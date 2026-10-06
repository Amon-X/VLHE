/*
 * vlhe_mod_render.c - Render MIDI to a file. Read vlhe_mod_render.h
 * for why this is a view rather than a settings page.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHAT IT DRIVES. `smf2wav FONT.sf2 FILE.mid OUT.wav' - the offline
 * renderer that already exists in vmidi/synth/ and shares every line
 * of its DSP with vmidid. So what comes out of this page is the same
 * audio the synth would have played, which is the whole point: a
 * user can render a file and hear exactly what the machine would
 * have produced live, on a machine too slow to produce it live.
 *
 * AND THAT IS NOT HYPOTHETICAL. CLAUDE.md records 86Box unable to run
 * vmidid's defaults at all - 795 xruns at 44100/64 voices, where the
 * same file renders offline with no deadline to miss.
 *
 * THE SOUNDFONT COMES FROM THE CONFIG, not from a third picker. The
 * MIDI page already has the font setting and vlhe_fonts() reports it;
 * asking again here would be a second place to set one thing.
 *
 * MP3 IS OPTIONAL AND EXTERNAL. LAME is not on any Corel disc and is
 * not ours to ship, so the checkbox is off unless the user names a
 * binary. The pipeline is smf2wav to a temporary .wav, then LAME to
 * the .mp3 - not a pipe, because smf2wav writes a RIFF header with a
 * length in it and cannot stream.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <sys/time.h>
#include <signal.h>

#include <gtk/gtk.h>

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_rates.h"
#include "vlhe_tip.h"
#include "vlhe_buttons.h"    /* vlhe_buttons_equalise */
#include "vlhe_conf.h"       /* vlhe_tmp_open */
#include "vlhe_filter.h"
#include "vlhe_self.h"
#include "vlhe_mod_render.h"

static void (*g_report)(const char *);

static GtkWidget *g_in;         /* the .mid path, an entry           */
static GtkWidget *g_out;        /* the output name, editable         */
static GtkWidget *g_mp3;        /* "encode to MP3 with LAME"         */
static GtkWidget *g_lame;       /* where LAME is, an entry           */
static GtkWidget *g_lame_row;   /* so it can grey out with the box   */
static GtkWidget *g_wavtemp;    /* where the temporary WAV goes: an
                                 * option menu, VLHE_WAVTEMP_* order */
static GtkWidget *g_wavtemp_row; /* greyed with the LAME row          */
static GtkWidget *g_font;       /* which soundfont, a label - the
                                 * MIDI page owns the setting        */
static GtkWidget *g_go;
static GtkWidget *g_stop;       /* Cancel, beside Render - live only
                                 * while a render runs               */
static int        g_cancel;     /* set by Cancel, read by the loops
                                 * in run_wait_progress()            */
static GtkWidget *g_picker;     /* one file dialog at a time         */

/*
 * THE ADVANCED TAB - every option smf2wav has that changes the
 * output, and a switch that says whether to use them at all.
 *
 * `Use VLHE settings' IS ON BY DEFAULT (the user, 2026-09-22) and
 * means: render with what Midi Settings holds, so the file sounds
 * like the machine. Unticked, the controls below take over and the
 * page becomes a comparison harness - which is what -F, -L and -B
 * are for in design/20 and 21.
 *
 * THE FONTS COME FROM THE MIDI PAGE, always. The user was explicit:
 * "drop downs for the fonts (as found by the midi page no extra
 * finder here)". A second font finder would be a second place to
 * put a soundfont, and vlhe_available_fonts() already reports what
 * every configured directory holds.
 */
static GtkWidget *g_use_vlhe;   /* the switch                        */
static GtkWidget *g_progress;   /* the Progress: box - what the status
                                 * bar says, on the page itself      */
static GtkWidget *g_adv_box;    /* everything it greys out           */
static GtkWidget *g_gm;         /* General MIDI font, a dropdown     */
static GtkWidget *g_song;       /* song font (bank 1), or none       */
static GtkWidget *g_voices;     /* 1..2048, smf2wav's offline build  */
static GtkWidget *g_rate;
static GtkWidget *g_gain;
static GtkWidget *g_reverb;     /* -E, apart since 2026-10-05        */
static GtkWidget *g_chorus;
static GtkWidget *g_bypass;     /* -B filter bypass                  */
static GtkWidget *g_modenv;     /* -M, design/54 D28 and D70         */
static GtkWidget *g_law;        /* -L volume law                     */
static GtkWidget *g_vfilter;    /* -F velocity filter                */

static int g_loading;           /* writing widgets - not an edit     */

/* What the font dropdowns choose from - the MIDI page's list. */
static char g_avail[VLHE_MAX_AVAIL][VLHE_PATH_MAX];
static int  g_navail;

/*
 * smf2wav's OFFLINE CEILING, NOT the daemon's.
 *
 * The daemon's RENDER_MAX_VOICES is 64 and every machine here
 * struggles with it - CLAUDE.md records 86Box managing 795 xruns at
 * that setting. The OFFLINE build overrides it: `smf2wav -p' accepts
 * 1 to 2048 and defaults to 512, because there is no deadline to
 * miss and the worst file in a 152-file corpus wants 409 voices
 * (design/21 section 15).
 *
 * SO THE TWO NUMBERS ARE NOT A MISTAKE, and a reader comparing this
 * page with Midi Settings will see 2048 here against 64 there.
 */
/*
 * BOTH ARE COPIES, AND THE SOURCE IS NOT A HEADER.
 *
 * `vmidi/synth/Makefile' has OFFLINE_VOICES = 2048, passed to
 * smf2wav as -DRENDER_MAX_VOICES on its own build line. The GUI
 * cannot include a build variable, so the number lives here and in
 * `vlhe_backend.h' as VLHE_RENDER_VOICES_MAX. THREE PLACES, and
 * raising the Makefile's alone changes only what smf2wav accepts.
 *
 * AND 512 IS A MEASURED VALUE, NOT A ROUND ONE. design/21 section 15
 * rendered a 152-file corpus and found `Digital Surf.mid' wanting
 * **409 voices** - more than TiMidity's 256 ceiling - with
 * `New Time.mid' at 187 and `gmstriving.mid' at 162. 512 is the first
 * sensible number above the corpus's worst case, so a default render
 * of anything in it never steals.
 *
 * NOTES ARE NOT VOICES, which is why 409 is surprising: peak
 * simultaneous NOTES never exceeds 29 anywhere in that corpus, and
 * the voices-to-notes ratio runs 2.0x to 7.7x with preset layering
 * and release tails.
 */
#define RENDER_VOICES_MAX 2048
#define RENDER_VOICES_DEF  512

/* The Advanced tab's readers, defined with the tab further down.
 * on_render() needs them and comes first, so the widgets stay next
 * to the code that builds them. */
static const char *font_chosen(GtkWidget *opt, int allow_none);
static void font_menu_fill(GtkWidget *opt, const char *none_label,
                           const char *current, int *sel_out);
static void font_label_refresh(void);
static void on_font_menu_changed(GtkWidget *w, gpointer d);
static int         choice_index(GtkWidget *opt);

/* THE RATE MENU'S ENTRIES ARE vlhe_rates.h's, in its order. A position
 * off the list - none chosen - is its first rate; a rate off the list
 * (an old config's 48000) selects the first. */
static int
rate_at(int i)
{
    static const int rates[] = VLHE_SYNTH_RATES;

    return (i >= 0 && i < VLHE_SYNTH_NRATES) ? rates[i] : rates[0];
}

static int
rate_index(int hz)
{
    static const int rates[] = VLHE_SYNTH_RATES;
    int i;

    for (i = 0; i < VLHE_SYNTH_NRATES; i++)
        if (rates[i] == hz)
            return i;
    return 0;
}
static void        lame_failed(int rc, const char *path);

/* Likewise the dirty marker: the builders connect it and the trio
 * that defines it reads the widgets, so one of the two has to be
 * declared ahead. */
static void mark_dirty(GtkWidget *w, gpointer d);

static void on_font_menu_changed(GtkWidget *w, gpointer d)
{
    mark_dirty(w, d);
    font_label_refresh();
}

static void say(const char *s)
{
    if (g_report != NULL)
        g_report(s);
    /*
     * AND ON THE PAGE. design/36 row 25 - the user's shape, 2026-09-24:
     * "Add a label with the text Progress: and to the right of it a
     * box that gives the same text as status bar". One string, two
     * places; the status bar is at the bottom of a window that may be
     * scrolled or partly covered, and a render is the one thing here
     * a user sits and watches.
     */
    if (g_progress != NULL)
        gtk_label_set_text(GTK_LABEL(g_progress), s);
}

/*
 * THE DEFAULT OUTPUT NAME: the input with its extension replaced.
 *
 * THE USER'S REQUIREMENT, 2026-09-22: "a text box for the output name
 * initially set to match the input file but allows the user to
 * change it". So this only ever fills the box when the user picks an
 * input - it never overwrites a name they have typed, which is
 * checked by the caller rather than here.
 *
 * SAME DIRECTORY AS THE INPUT. A render of /root/midi/song.mid lands
 * beside it rather than in whatever directory the GUI was started
 * from, which is where a user would look for it.
 */
static void
default_output(const char *in, const char *ext, char *out, size_t max)
{
    const char *dot;
    size_t n;

    if (in == NULL || *in == '\0' || max == 0) {
        if (max > 0)
            out[0] = '\0';
        return;
    }

    /* THE LAST DOT AFTER THE LAST SLASH - a directory with a dot in
     * its name must not eat the filename's extension. */
    dot = strrchr(in, '.');
    {
        const char *slash = strrchr(in, '/');
        if (dot != NULL && slash != NULL && dot < slash)
            dot = NULL;
    }

    n = dot != NULL ? (size_t)(dot - in) : strlen(in);
    if (n + strlen(ext) + 1 > max)
        n = max - strlen(ext) - 1;
    memcpy(out, in, n);
    strcpy(out + n, ext);
}

/* Swap the output's extension when the MP3 box is toggled, but only
 * when it still looks like one we put there - a name the user typed
 * is theirs. */
static void
retarget_extension(void)
{
    const char *cur;
    const char *in;
    char want[VLHE_PATH_MAX];
    char other[VLHE_PATH_MAX];
    int  mp3;

    if (g_out == NULL || g_in == NULL || g_mp3 == NULL)
        return;

    cur = gtk_entry_get_text(GTK_ENTRY(g_out));
    in  = gtk_entry_get_text(GTK_ENTRY(g_in));
    mp3 = GTK_TOGGLE_BUTTON(g_mp3)->active;

    default_output(in, mp3 ? ".mp3" : ".wav", want, sizeof want);
    default_output(in, mp3 ? ".wav" : ".mp3", other, sizeof other);

    /* Empty, or exactly what we would have suggested for the OTHER
     * setting - either way it is ours to change. */
    if (cur[0] == '\0' || strcmp(cur, other) == 0)
        gtk_entry_set_text(GTK_ENTRY(g_out), want);
}

static void
on_mp3_toggled(GtkWidget *w, gpointer data)
{
    int on = GTK_TOGGLE_BUTTON(w)->active;

    (void)data;
    if (g_lame_row != NULL)
        gtk_widget_set_sensitive(g_lame_row, on ? TRUE : FALSE);
    if (g_wavtemp_row != NULL)
        gtk_widget_set_sensitive(g_wavtemp_row, on ? TRUE : FALSE);
    retarget_extension();
}

/* ------------------------------------------------------------------ */
/* Picking the input                                                  */
/* ------------------------------------------------------------------ */

static void
on_picker_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    g_picker = NULL;
}

static void
on_picker_cancel(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (g_picker != NULL)
        gtk_widget_destroy(g_picker);
}

static void
on_picker_ok(GtkWidget *w, gpointer data)
{
    const char *path;
    char        def[VLHE_PATH_MAX];

    (void)w;
    (void)data;
    if (g_picker == NULL)
        return;

    path = gtk_file_selection_get_filename(GTK_FILE_SELECTION(g_picker));
    if (path != NULL && *path != '\0') {
        struct stat st;

        /* A DIRECTORY IS NOT A CHOICE. GtkFileSelection returns the
         * current directory when nothing is selected, and rendering
         * one would fail obscurely inside smf2wav. */
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            say(STR_RND_MSG_DIRECTORY_PICK_MID_FILE);
            return;
        }

        gtk_entry_set_text(GTK_ENTRY(g_in), path);

        /*
         * FILL THE OUTPUT NAME FROM THE NEW INPUT, ALWAYS.
         *
         * THE USER ASKED FOR IT "initially set to match the input
         * file but allows the user to change it", and picking a
         * different input is a new `initially' - a box still naming
         * the PREVIOUS file would be worse than one that follows,
         * because the mismatch is easy to miss and the render would
         * quietly overwrite the wrong thing.
         *
         * A first version tried to preserve a name the user had
         * typed, by testing whether the box still held our suggestion
         * for the previous input. It cannot: the previous input is
         * gone by the time we get here, so there was nothing to
         * compare against and the code did the same thing in both
         * branches while the comment claimed otherwise.
         *
         * So: follow the input, and the user edits afterwards if they
         * want something else. The box is right there.
         */
        {
            int mp3 = g_mp3 != NULL &&
                      GTK_TOGGLE_BUTTON(g_mp3)->active;

            default_output(path, mp3 ? ".mp3" : ".wav", def, sizeof def);
            gtk_entry_set_text(GTK_ENTRY(g_out), def);
        }
        say("");
    }
    gtk_widget_destroy(g_picker);
}

static void
on_browse(GtkWidget *w, gpointer data)
{
    GtkWidget  *sel;
    const char *cur;

    (void)w;
    (void)data;

    if (g_picker != NULL) {
        gdk_window_raise(g_picker->window);
        return;                 /* one at a time */
    }

    sel = gtk_file_selection_new(STR_RND_TITLE_SELECT_MIDI_FILE);
    vlhe_filesel_fit(sel);         /* a long path must not widen it */

    /*
     * NO Create / Rename / Delete. GTK 1.2 shows those three by
     * default and this dialog is a CHOOSER - the user, 2026-09-22,
     * asking whether they were needed. They are not:
     *
     *   - creating and renaming belong to a file manager, and Corel
     *     ships one
     *   - DELETE IS A DESTRUCTIVE CONTROL IN A WINDOW SOMEBODY
     *     OPENED TO PICK A FILE, with only GTK's own small
     *     confirmation between it and a lost soundfont
     *   - and they cost vertical space design/32 measured as already
     *     tight at 800x600
     */
    gtk_file_selection_hide_fileop_buttons(GTK_FILE_SELECTION(sel));
    g_picker = sel;

    /* START WHERE THEY LEFT OFF, if anywhere. */
    cur = gtk_entry_get_text(GTK_ENTRY(g_in));
    if (cur != NULL && *cur != '\0')
        gtk_file_selection_set_filename(GTK_FILE_SELECTION(sel), cur);

    /*
     * A "Files of type" DROPDOWN, not a one-shot glob.
     *
     * A first version called gtk_file_selection_complete() once at
     * open, which the user found on 2026-09-22: "as soon as you walk
     * a directory the filter vanishes", with the pattern left sitting
     * in the filename box. That function is not a filter - it sets
     * the entry text and populates ONCE - and GTK 1.2 has no filter
     * API at all. vlhe_filter.c is ours; design/32 measured how a
     * year earlier and this is the first use of it.
     *
     * `All files' IS THE DEFAULT, by that document's reasoning: a
     * filter that hides the file the user came for leaves an EMPTY
     * list with nothing saying why, which reads as a broken picker.
     * A file that is not MIDI is caught on OK instead, where there
     * is room to say so.
     *
     * ONE PATTERN PER ENTRY - every separator was measured to return
     * zero rows, so `*.mid*' is the form, and it covers .mid and
     * .midi as the user pointed out. NOT .mus: smf2wav cannot read
     * MUS, and offering it would offer a choice that always fails.
     */
    {
        /*
         * MIDI FIRST AND SELECTED - the user's call, 2026-09-22:
         * "the default filter should be mid/midi not all files".
         *
         * IT WAS THE OTHER WAY ROUND AND THAT WAS INCONSISTENT with
         * the CD page's picker, which defaults to disc images for
         * exactly the reason that applies here: a filter the user has
         * to go and switch on is a filter most will never find, and
         * this dialog exists to pick a MIDI file.
         *
         * `*.mid*' CATCHES BOTH SPELLINGS - .mid and .midi - and the
         * matcher is case-insensitive, so .MID off an ISO-9660 disc
         * matches too. The user's own guidance when the filter was
         * built: "you can just filter mid* since that will pick mid
         * or midi files".
         *
         * ALL FILES STAYS, LAST. smf2wav will render any SMF whatever
         * it is called, and someone with a .kar or a name from a DOS
         * machine needs a way through.
         */
        static const struct vlhe_filter midi_types[] = {
            { STR_RND_FILTER_MIDI, "*.mid*" },
            { STR_RND_FILTER_ALL,              NULL     },
            { NULL, NULL }
        };

        vlhe_filter_attach(sel, midi_types, 0);
    }

    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->ok_button),
                       "clicked", GTK_SIGNAL_FUNC(on_picker_ok), NULL);
    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->cancel_button),
                       "clicked", GTK_SIGNAL_FUNC(on_picker_cancel), NULL);
    gtk_signal_connect(GTK_OBJECT(sel), "destroy",
                       GTK_SIGNAL_FUNC(on_picker_destroy), NULL);

    gtk_widget_show(sel);
}

/* ------------------------------------------------------------------ */
/* Running the render                                                 */
/* ------------------------------------------------------------------ */

/*
 * WHERE smf2wav IS - and a bare name was not enough.
 *
 * The user on 86Box, 2026-09-22: "sm2wav is not in path so I cant
 * test rendering". It is STAGED, in /mnt/xfer beside the GUI, and
 * execvp() searches PATH which does not include the transfer mount.
 *
 * SO THE BINARY'S OWN DIRECTORY IS TRIED FIRST, which is the same
 * answer vlhe_apply.c's daemon_dir() reaches for the daemons: a
 * bundle is unpacked and run in place, and the thing beside us is
 * the thing we came with. Then the installed locations, then the
 * bare name so PATH still works where it is set up properly.
 */
static const char *
smf2wav_path(void)
{
    static char path[VLHE_PATH_MAX];
    static int  looked;
    static const char *const where[] = {
        "/usr/local/bin/smf2wav",
        "/usr/bin/smf2wav",
        NULL
    };
    struct stat st;
    int i;

    if (looked)
        return path;
    looked = 1;

    /*
     * THE PORTABLE TREE FIRST, AND IT USED TO BE `./smf2wav'.
     *
     * Found on target 2026-09-23: the user ran /mnt/xfer/vlhe.gtk
     * from `/', and the page said "smf2wav is not on the PATH" -
     * because `.' was `/' and the tool is in /mnt/xfer. The same
     * working-directory bug this evening removed from the config,
     * the journal and the module search, still sitting in the one
     * place nothing else touched.
     *
     * `Tools=' IN THE MARKER IS WHY IT IS ONE CALL. The tree can put
     * its programs in a subdirectory and this follows without
     * knowing, which is what the key was added for.
     */
    {
        char dir[VLHE_PATH_MAX];

        /* AND ONLY THERE, FOR A PORTABLE BUILD - design/55 13f,
         * 2026-10-04: its tools/ is the answer whether or not the file
         * is there, never the installed smf2wav below. */
        if (vlhe_self_is_trial()) {
            if (vlhe_self_subdir("Tools", dir, sizeof dir)
                && strlen(dir) + sizeof "/smf2wav" < sizeof path)
                sprintf(path, "%s/smf2wav", dir);
            else
                strcpy(path, "smf2wav");
            return path;
        }
    }

    for (i = 0; where[i] != NULL; i++) {
        if (stat(where[i], &st) == 0 && S_ISREG(st.st_mode)
            && access(where[i], X_OK) == 0) {
            strcpy(path, where[i]);
            return path;
        }
    }

    /* NOT FOUND ANYWHERE WE KNOW - hand execvp the bare name so a
     * machine with it properly on PATH still works, and let the
     * failure report itself. */
    strcpy(path, "smf2wav");
    return path;
}

/*
 * ONE LINE OF smf2wav's STDOUT - `progress N' becomes the status
 * message, anything else is ignored. Split out of run_wait_progress()
 * when its read loop changed shape for Cancel, so the loop reads as a
 * loop.
 */
static void
progress_say(const char *line, unsigned long total, const char *what)
{
    unsigned long done;

    if (sscanf(line, "progress %lu", &done) == 1) {
        char msg[96];
        /* done/total x 100, not done x 100 / total: the
         * latter wrapped a 32-bit long past 42.9 million
         * frames - 16 minutes at 44100 (design/47 R2). */
        int  pct = total >= 100UL
                   ? (int)(done / (total / 100UL))
                   : (int)(done * 100UL / total);

        if (pct > 100)
            pct = 100;      /* the estimate was low */

        /*
         * BOTH NUMBERS, NOT JUST THE TOTAL - the user,
         * 2026-09-22: "can it display how much is writen
         * so 15mb of approx 29MB kind of thing?"
         *
         * The first version printed the percentage and
         * the estimated total, which makes the reader do
         * the arithmetic to learn the one thing they
         * actually want on a slow machine: is it still
         * moving, and by how much.
         *
         * TENTHS ON THE WRITTEN FIGURE, whole megabytes
         * on the estimate. The first changes every second
         * and a bare integer would sit still for twelve
         * of them at this rate; the second is an estimate
         * and decimals on it would claim a precision it
         * does not have. "approx" says the same in words,
         * and is the user's wording.
         *
         * NO FLOATING POINT IN THE FORMAT. The target's
         * sh-utils segfaults on a width after a numeric
         * conversion (design/07) and this is a GUI rather
         * than a shell, but integer tenths cost one
         * divide and cannot be got wrong.
         */
        sprintf(msg,
                FMT_RND_U_U_MB_APPROX,
                what, pct,
                done * 4UL / 1048576UL,
                (done * 4UL % 1048576UL) * 10UL / 1048576UL,
                total * 4UL / 1048576UL);
        say(msg);
    }
}

/*
 * STOP THE CHILD - Cancel's half. SIGTERM, a short wait for it to go,
 * then SIGKILL; smf2wav has no handler so the first one is enough,
 * and LAME is given the same two seconds either way. `*st' receives
 * the status so the caller's reap is not run twice.
 *
 * GUARDED ON pid > 0, AND ON THE PID fork() RETURNED, never a field
 * anything else can reset - CLAUDE.md section 1, after kill(-1) took
 * the user's X session twice. The pid is a parameter so there is
 * nothing else it could be.
 */
static void
cancel_child(pid_t pid, int *st)
{
    int i;

    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    for (i = 0; i < 20; i++) {          /* 20 x 100 ms */
        struct timeval tv;

        if (waitpid(pid, st, WNOHANG) == pid)
            return;
        tv.tv_sec  = 0;
        tv.tv_usec = 100000;
        select(0, NULL, NULL, NULL, &tv);
    }
    kill(pid, SIGKILL);
    waitpid(pid, st, 0);
}

/*
 * RUN ONE COMMAND AND WAIT. fork + execvp, NO SHELL - a soundfont or
 * a MIDI file with a space in its path is ordinary, and passing any
 * of this through /bin/sh would need quoting nobody gets right.
 * vlhe_apply.c makes the same choice for the same reason.
 *
 * `total' > 0 TURNS ON PROGRESS: the child's stdout is read a line at
 * a time, `progress N' is turned into a percentage of `total' frames,
 * and the GUI is pumped between reads so the window repaints and the
 * status bar moves. 0 keeps the old behaviour - stdout to /dev/null
 * and a plain blocking wait - which is what LAME gets.
 *
 * WHY THE GUI HAS TO BE PUMPED AT ALL. The old version blocked in
 * waitpid() for the whole render, so nothing repainted: on a Pentium
 * II a 2048-voice render is minutes of a frozen window with no
 * indication it is alive. The user asked for a progress line,
 * 2026-09-22, and the read loop is what makes one possible.
 *
 * Returns the exit status, -1 if the program could not be run, or
 * RUN_CANCELLED if Cancel stopped it.
 *
 * `keep', IF GIVEN, RECEIVES THE PATH OF THE STDERR CAPTURE WHEN THE
 * CHILD FAILED, and the file is then LEFT for the caller - the user,
 * 2026-10-02, after a second attempt at parsing LAME's last line
 * failed: "Would it be easier to pipe the error to a file in /tmp and
 * point to that instead of trying to parse and strip the error
 * messages since you are matching on specific version of lame". The
 * caller shows it whole (lame_failed()) and names the path. On
 * success or cancel the file goes as before, so the only litter is
 * one small private file per FAILED encode, and the message says
 * where it is.
 */
#define RUN_CANCELLED (-2)
/*
 * `err' RECEIVES THE CHILD'S LAST LINE OF STDERR, or is left empty.
 *
 * WHY IT IS A TEMPORARY FILE AND NOT A SECOND PIPE. The progress loop
 * below reads stdout alone; a child that filled a stderr pipe while
 * we were not reading it would block forever, and both sides would
 * wait. A file cannot deadlock. smf2wav writes a handful of lines, so
 * the cost is nothing.
 *
 * WHY IT IS NEEDED AT ALL - the user, 2026-09-22, having just watched
 * a real out-of-space render fail. The message said "the render
 * failed - out of disk space, or not a MIDI file", naming two
 * unrelated causes because every non-zero exit but 2 and 127 mapped
 * to one string. smf2wav KNEW which it was and had said so, to a
 * stderr this function was sending to /dev/null.
 */
static int
run_wait_progress(char *const argv[], unsigned long total,
                  const char *what, char *err, int errmax,
                  char *keep, int keepmax)
{
    pid_t pid;
    int   st, fd[2];
    char  errfile[64];
    int   errfd = -1;
    int   rc;
    int   cancelled = 0;

    if (err != NULL && errmax > 0)
        err[0] = '\0';
    if (keep != NULL && keepmax > 0)
        keep[0] = '\0';

    if (total > 0 && pipe(fd) != 0)
        total = 0;              /* no pipe, no progress - still render */

    /*
     * A FRESH PRIVATE FILE, NEVER ONE THAT WAS THERE - design/48 S6.
     * This was `/tmp/vlhe-render-err.<pid>' opened O_CREAT|O_TRUNC:
     * a predictable name, followed if it was a link, and written as
     * root whenever the control centre runs as root, which on these
     * machines is the normal login. On failure errfile comes back
     * empty and the render simply runs without an error capture.
     */
    errfile[0] = '\0';
    if (err != NULL)
        errfd = vlhe_tmp_open(errfile, sizeof errfile, "vlhe-render-err");

    pid = fork();
    if (pid < 0) {
        if (total > 0) { close(fd[0]); close(fd[1]); }
        return -1;
    }

    if (pid == 0) {
        /*
         * STDERR ALWAYS GOES TO /dev/null - smf2wav is chatty there
         * and this is a GUI. STDOUT goes to the pipe when we want
         * progress and to /dev/null otherwise, which is why the two
         * were split in smf2wav: a diagnostic cannot be misread as a
         * percentage.
         */
        int null = open("/dev/null", O_WRONLY);

        if (total > 0) {
            close(fd[0]);
            dup2(fd[1], 1);
            close(fd[1]);
        } else if (null >= 0) {
            dup2(null, 1);
        }
        if (errfd >= 0)
            dup2(errfd, 2);
        else if (null >= 0)
            dup2(null, 2);
        if (null >= 0 && null > 2)
            close(null);
        if (errfd > 2)
            close(errfd);
        execvp(argv[0], argv);
        _exit(127);
    }

    if (total > 0) {
        /*
         * select() AND read(), NOT fgets() - so Cancel is seen within
         * half a second. fgets blocked between progress lines, which
         * come once per second of RENDERED audio: on a slow machine at
         * a high voice count that is several seconds of wall clock
         * during which a click on Cancel sat unread. The LAME loop
         * below already waits this way; now both stages do.
         *
         * A raw read() rather than a FILE, because select() cannot
         * see what stdio has already buffered - a line sitting in the
         * FILE's buffer would wait for the NEXT one to arrive before
         * being shown. Lines are assembled here instead; EOF is what
         * says the child has finished writing, so no timer decides
         * that.
         *
         * THE MESSAGE SAYS "about", because the total is an ESTIMATE
         * and the user was explicit that it must read as one: "needs
         * estimated or approximate since we dont know the file size
         * but it is a rough estimate".
         */
        char acc[512];
        int  alen = 0, eof = 0;

        close(fd[1]);
        while (!eof) {
            fd_set         rf;
            struct timeval tv;

            FD_ZERO(&rf);
            FD_SET(fd[0], &rf);
            tv.tv_sec  = 0;
            tv.tv_usec = 500000;
            if (select(fd[0] + 1, &rf, NULL, NULL, &tv) > 0) {
                int got = read(fd[0], acc + alen,
                               sizeof acc - 1 - (size_t) alen);

                if (got <= 0) {
                    eof = 1;
                } else {
                    char *s = acc, *nl;

                    alen += got;
                    acc[alen] = '\0';
                    while ((nl = strchr(s, '\n')) != NULL) {
                        *nl = '\0';
                        progress_say(s, total, what);
                        s = nl + 1;
                    }
                    alen = (int) strlen(s);
                    memmove(acc, s, (size_t) alen + 1);
                    if (alen >= (int) sizeof acc - 1)
                        alen = 0;   /* a line longer than the buffer
                                     * is not one of ours - dropped */
                }
            }
            while (gtk_events_pending())
                gtk_main_iteration();
            if (g_cancel) {
                cancel_child(pid, &st);
                cancelled = 1;
                break;
            }
        }
        close(fd[0]);
    }

    /*
     * WATCH THE STDERR FILE WHILE IT RUNS, when the caller asked for
     * progress and the child writes none on stdout.
     *
     * THAT IS LAME. It has `--disptime N', which prints a table with
     * a percentage and an ETA - but on STDERR, and separated by
     * CARRIAGE RETURNS rather than newlines, because it is drawing
     * over one line for a terminal. fgets() would block until the
     * whole encode finished.
     *
     * SO THE FILE IS POLLED, not the pipe. It is a real file (see
     * above - a second pipe would deadlock against the stdout loop),
     * so reading what has been written so far costs a seek and a
     * read, and the child is not blocked by our not keeping up.
     *
     * HALF A SECOND is chosen against `--disptime 1': fast enough
     * that the line never looks stuck, slow enough that a Pentium II
     * is not woken for nothing.
     */
    if (total == 0 && what != NULL && errfd >= 0) {
        int done = 0;

        while (!done) {
            char   buf[1024];
            off_t  end;

            /* HAS IT FINISHED? Asked before sleeping, so a fast
             * encode is not held up by the poll interval. */
            if (pid > 0 && waitpid(pid, &st, WNOHANG) == pid) {
                done = 1;
            } else {
                struct timeval tv;

                tv.tv_sec  = 0;
                tv.tv_usec = 500000;
                select(0, NULL, NULL, NULL, &tv);
            }

            /* THE LAST LINE SO FAR. Seeking to the end and backing
             * up is how you read a growing file without holding a
             * position across a write. */
            end = lseek(errfd, 0L, SEEK_END);
            if (end > 0) {
                long start = end > (off_t) sizeof buf
                             ? (long)(end - sizeof buf) : 0L;
                int  got;

                lseek(errfd, start, SEEK_SET);
                got = read(errfd, buf, sizeof buf - 1);
                if (got > 0) {
                    char *p, *last = NULL;

                    buf[got] = '\0';
                    /* SPLIT ON BOTH, since LAME uses \r and its
                     * banner lines use \n. */
                    for (p = buf; *p != '\0'; p++)
                        if (*p == '\r' || *p == '\n') {
                            *p = '\0';
                            if (p[1] != '\0')
                                last = p + 1;
                        }

                    /*
                     * ONLY A PROGRESS LINE, and the test is that it
                     * STARTS WITH A DIGIT.
                     *
                     * MATCHING ON `%' WAS NOT ENOUGH - measured
                     * against real output. LAME's rows look like
                     *
                     *     9/2091   ( 0%)|  0:00/0:00| ...
                     *
                     * and its closing summary is
                     *
                     *     average: 128.0 kbps LR: 51 (2.438%) ...
                     *
                     * which also has a `%' and would have been shown
                     * as the last "progress" of every encode. The
                     * frame count is what distinguishes them.
                     */
                    if (last != NULL) {
                        while (*last == ' ')
                            last++;
                        if (*last >= '0' && *last <= '9') {
                            char msg[128];

                            /*
                             * NOT LAME'S LINE VERBATIM - design/36 row 25. It reached the
                             * status bar as
                             *
                             *   encoding... 590/3826 (15%)|  0:09/  0:59|  0:09/  0:59|  1.6734x|  0:50
                             *
                             * - the same pair of times twice with no labels (elapsed/total
                             * and playing-time elapsed/total), LAME's column bars, and a
                             * speed with no unit. The render phase above is written for a
                             * person; the encode phase was written for a terminal. The user:
                             * "its possible we dont need the entire output but if it can be
                             * displayed correctly." So: frames, percent, and the remaining
                             * time, which are the three things anyone waiting wants. If the
                             * line is not LAME's shape - another encoder, a warning - it is
                             * shown as it came, which is what happened before.
                             */
                            {
                                unsigned long fdone, ftotal;
                                int pct, m1, s1, m2, s2, m3, s3, m4, s4, mr = -1, sr = -1;
                                int nf;
                                char xs[16];
                            
                                nf = sscanf(last, "%lu/%lu (%d%%)|%d:%d/%d:%d|%d:%d/%d:%d|%15[^|]|%d:%d",
                                           &fdone, &ftotal, &pct, &m1, &s1, &m2, &s2,
                                           &m3, &s3, &m4, &s4, xs, &mr, &sr);
                                if (nf >= 3) {
                                    if (nf == 14 && (mr > 0 || sr > 0))
                                        sprintf(msg, FMT_RND_U_U_FRAMES_ABOUT,
                                                what, pct, fdone, ftotal, mr, sr);
                                    else
                                        sprintf(msg, FMT_RND_U_U_FRAMES,
                                                what, pct, fdone, ftotal);
                                } else {
                                    sprintf(msg, "%.20s... %.90s", what, last);
                                }
                            }
                            say(msg);
                        }
                    }
                }
            }

            while (gtk_events_pending())
                gtk_main_iteration();

            if (!done && g_cancel) {
                cancel_child(pid, &st);
                cancelled = 1;
                break;
            }

            /* A CHILD THAT VANISHED - waitpid would have said so,
             * but a loop with no exit is worse than a redundant
             * guard. */
            if (done || pid <= 0)
                break;
        }

        /* The wait below must not block on a child already reaped. */
        if (done) {
            rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            goto reaped;
        }
    }

    /* A CANCELLED CHILD WAS REAPED BY cancel_child() - waiting again
     * would hang on a pid that is no longer ours. */
    if (cancelled) {
        rc = RUN_CANCELLED;
        goto reaped;
    }

    /* GUARDED ON pid > 0 even though fork() just returned it -
     * CLAUDE.md records kill(-1) taking the user's X session twice,
     * and waitpid has the same sign convention. */
    if (pid > 0 && waitpid(pid, &st, 0) == pid)
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    else
        rc = -1;

reaped:

    /*
     * THE LAST LINE IT SAID, not the first. smf2wav's diagnostics end
     * with the one that matters - "write failed - No space left on
     * device" comes after the render summary - so the tail is the
     * useful part.
     *
     * A LINE ENDS AT \r AS WELL AS \n, character by character so a
     * buffer boundary cannot cut a segment, and the segment open at
     * EOF counts. THAT WAS AN ATTEMPT TO ISOLATE LAME'S ERROR and it
     * did not work: LAME puts the carriage return at the START of
     * each --disptime row, so its last row and "Error writing mp3
     * output" are one segment with nothing between them (the user's
     * screenshot, 2026-10-02, after this change). The reader is kept
     * because it is right for smf2wav, whose diagnostics are ours;
     * LAME's output is no longer parsed at all - see `keep' above
     * and lame_failed().
     *
     * THE FILE IS REMOVED EITHER WAY. It is ours, in /tmp, named
     * after our pid; one left behind per failed render would be a
     * slow litter.
     */
    if (errfd >= 0) {
        if (err != NULL && errmax > 1) {
            char  line[256];
            int   len = 0, c;
            FILE *ef;

            lseek(errfd, 0L, SEEK_SET);
            ef = fdopen(errfd, "r");
            if (ef != NULL) {
                for (;;) {
                    c = fgetc(ef);
                    if (c == EOF || c == '\n' || c == '\r') {
                        if (len > 0) {
                            line[len] = '\0';
                            strncpy(err, line, errmax - 1);
                            err[errmax - 1] = '\0';
                        }
                        len = 0;
                        if (c == EOF)
                            break;
                    } else if (len < (int) sizeof line - 1) {
                        line[len++] = (char) c;
                    }
                }
                fclose(ef);         /* closes errfd with it */
                errfd = -1;
            }
        }
        if (errfd >= 0)
            close(errfd);
        /* ONLY WHAT WE MADE - an empty name means the open failed,
         * and removing a /tmp name we did not create would, as root,
         * delete whatever someone else left there. KEPT when the
         * caller asked and the child failed (not cancelled, not 127 -
         * nothing ran, so nothing was said). */
        if (errfile[0] != '\0') {
            if (keep != NULL && keepmax > (int) strlen(errfile)
                && rc != 0 && rc != RUN_CANCELLED && rc != 127)
                strcpy(keep, errfile);
            else
                unlink(errfile);
        }
    }

    return rc;
}



/*
 * HOW BIG WILL IT BE, AND IS THERE ROOM?
 *
 * THE ESTIMATE COMES FROM smf2wav ITSELF, via `-n', rather than being
 * computed here. That is the whole point: the length of a MIDI file
 * is its tempo map integrated, plus the releases, plus the effects
 * tail, clamped to the runaway ceiling - and smf2wav is the program
 * that knows every one of those. A second copy in the GUI would be a
 * second copy to keep in step, and this project has the scars.
 *
 * SO THE ARGUMENTS ARE THE RENDER'S OWN, with -n prepended. The
 * settings therefore cannot disagree with what is about to run - the
 * user's requirement, 2026-09-22: "all the calculations for the size
 * needs to take the settings in consideration".
 *
 * Returns frames on success, 0 if the estimate could not be made -
 * in which case the caller renders anyway rather than blocking on a
 * number it does not have.
 */
static unsigned long
render_estimate(char *const argv[], unsigned long *bytes,
                unsigned long *secs_out, int *capped_out)
{
    char *est[36];
    int   i, n = 0, fdp[2];
    pid_t pid;
    unsigned long secs = 0, by = 0;
    int   st, capped = 0;

    *bytes = 0;
    *secs_out = 0;
    *capped_out = 0;

    est[n++] = argv[0];
    est[n++] = "-n";
    for (i = 1; argv[i] != NULL && n < 34; i++)
        est[n++] = argv[i];
    est[n] = NULL;

    if (pipe(fdp) != 0)
        return 0;

    pid = fork();
    if (pid < 0) {
        close(fdp[0]); close(fdp[1]);
        return 0;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        close(fdp[0]);
        dup2(fdp[1], 1);
        close(fdp[1]);
        if (null >= 0) { dup2(null, 2); if (null > 2) close(null); }
        execvp(est[0], est);
        _exit(127);
    }

    close(fdp[1]);
    {
        FILE *fp = fdopen(fdp[0], "r");
        char  buf[128];

        if (fp != NULL) {
            while (fgets(buf, sizeof buf, fp) != NULL) {
                sscanf(buf, "estimate_seconds %lu", &secs);
                sscanf(buf, "estimate_bytes %lu", &by);
                sscanf(buf, "capped %d", &capped);
            }
            fclose(fp);
        } else {
            close(fdp[0]);
        }
    }

    if (pid > 0 && waitpid(pid, &st, 0) == pid
        && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        *bytes = by;
        *secs_out = secs;
        *capped_out = capped;
        return by / 4UL;                /* frames */
    }
    return 0;
}

/*
 * FREE SPACE ON THE FILESYSTEM THAT WILL HOLD `path'.
 *
 * statfs() ON THE DIRECTORY, not the file - the file does not exist
 * yet. f_bavail rather than f_bfree, because the reserved blocks are
 * not ours to spend: on a default ext2 that is 5%, and a render that
 * eats into root's reserve is a render that has already gone wrong.
 *
 * Returns 0 when it cannot tell, which the caller reads as "do not
 * warn" - refusing to render because we could not stat something
 * would be worse than the problem.
 */
static unsigned long
free_bytes(const char *path)
{
    struct statfs sfs;
    char  dir[VLHE_PATH_MAX];
    char *slash;

    strncpy(dir, path, sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == NULL)
        strcpy(dir, ".");
    else if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';

    if (statfs(dir, &sfs) != 0)
        return 0;
    /*
     * CLAMPED, NOT WRAPPED - design/47 R2. The product is 64-bit on
     * the host and 32-bit on the target, where blocks x block size
     * wrapped above 4 GiB free and raised a false "Not enough
     * space?". A render needs well under a gigabyte; anything above
     * what fits is reported as "at least that", which every caller
     * reads correctly.
     */
    {
        unsigned long blocks = (unsigned long) sfs.f_bavail;
        unsigned long bsize  = (unsigned long) sfs.f_bsize;

        if (bsize != 0 && blocks > ~0UL / bsize)
            return ~0UL;
        return blocks * bsize;
    }
}


static void space_ok(GtkWidget *w, gpointer data)
{
    (void)w;
    *(int *)data = 1;
}

/*
 * WARN, DO NOT BLOCK - the user's call, 2026-09-22: "The calculations
 * could be wrong so I dont want to block a user from writing a file
 * when there is space".
 *
 * So this appears only when the estimate EXCEEDS the free space, and
 * even then Render is one click away. An estimate that is wrong high
 * costs a dialog; one that is wrong low costs the failure it exists
 * to prevent - which is why smf2wav's `-n' rounds the release
 * allowance up.
 *
 * AND IT SAYS "estimated". The user was explicit that the number must
 * not read as a measurement.
 */
static int
confirm_space(unsigned long need, unsigned long have)
{
    GtkWidget *dlg, *lab, *btn;
    static int answer;
    char       msg[384];

    answer = 0;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_RND_TITLE_NOT_ENOUGH_SPACE);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    sprintf(msg,
        FMT_RND_RENDER_ESTIMATED_ABOUT_U,
        (need + 524288UL) / 1048576UL,
        have / 1048576UL);

    lab = gtk_label_new(msg);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_RND_BTN_RENDER_ANYWAY), STR_RND_BTN_RENDER_ANYWAY_TIP);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(space_ok), (gpointer)&answer);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);

    /* CANCEL TAKES THE FOCUS. The same reasoning as confirm_quit's
     * "Keep editing": Return should not start something expensive. */
    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
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

    return answer;
}

/*
 * A LONG RENDER IS CONFIRMED FIRST - 2026-10-04, a second agent's point,
 * agreed by the user. The 20 minutes that used to cut every render short
 * is now the point at which the page ASKS, showing length and size: a
 * real long piece is one click, and a file claiming 58 minutes for a
 * three-minute song is seen before anything is written. Past the cap
 * (smf2wav's `capped') it also says the render will stop there. Cancel
 * takes the focus, as in confirm_space().
 */
#define RENDER_ASK_SECONDS  1200UL

static int
confirm_long(unsigned long secs, unsigned long bytes, int capped)
{
    GtkWidget *dlg, *lab, *btn;
    static int answer;
    char       msg[512];

    answer = 0;
    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_RND_TITLE_LONG_RENDER);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    sprintf(msg, capped ? FMT_RND_LONG_RENDER_CAPPED : FMT_RND_LONG_RENDER,
            secs / 60UL, secs % 60UL, (bytes + 524288UL) / 1048576UL);
    lab = gtk_label_new(msg);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_label_set_line_wrap(GTK_LABEL(lab), TRUE);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_widget_set_usize(lab, 380, -1);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_RND_BTN_RENDER_IT), STR_RND_BTN_RENDER_IT_TIP);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(space_ok), (gpointer)&answer);
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
    vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();
    return answer;
}

static int (*g_before_cb)(void);

static void render_body(GtkWidget *w, gpointer data);

/*
 * ONE RENDER AT A TIME - design/47 R1. run_wait_progress() pumps GTK
 * events while the child runs, so a second click on Render started a
 * NESTED render into the same output file. The button is greyed for
 * the duration and a click that arrives anyway is refused; every one
 * of render_body()'s returns comes back through here.
 */
static int g_rendering;

static void
on_render(GtkWidget *w, gpointer data)
{
    if (g_rendering) {
        say(STR_RND_MSG_RENDER_ALREADY_RUNNING);
        return;
    }
    g_rendering = 1;
    g_cancel = 0;
    if (g_go != NULL)
        gtk_widget_set_sensitive(g_go, FALSE);
    if (g_stop != NULL)
        gtk_widget_set_sensitive(g_stop, TRUE);
    render_body(w, data);
    if (g_stop != NULL)
        gtk_widget_set_sensitive(g_stop, FALSE);
    if (g_go != NULL)
        gtk_widget_set_sensitive(g_go, TRUE);
    g_rendering = 0;
}

/*
 * CANCEL - the user, 2026-10-02, after a render that could only be
 * waited out. Sets the flag; run_wait_progress() sees it on its next
 * pump, within half a second, and stops whichever child is running.
 * The button is live only while a render is, so a click here always
 * has something to stop.
 */
int
render_abort(void)
{
    if (!g_rendering)
        return 0;
    g_cancel = 1;
    return 1;
}

static void
on_cancel(GtkWidget *w, gpointer data)
{
    (void) w; (void) data;
    g_cancel = 1;
    say(STR_RND_MSG_CANCELLING);
}

static void
render_body(GtkWidget *w, gpointer data)
{
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    const char *in, *out, *lame;
    /* COPIES, NOT THE ENTRIES' OWN STORAGE - design/47 R3. The three
     * pointers from gtk_entry_get_text() were held across the event
     * pump in run_wait_progress(), and an edit to a field mid-render
     * frees what they point at. */
    char  in_buf[VLHE_PATH_MAX], out_buf[VLHE_PATH_MAX];
    char  lame_buf[VLHE_PATH_MAX];
    struct stat before;         /* `out' before LAME, for the failure
                                 * branch */
    int   had_out = 0;
    char  wav[VLHE_PATH_MAX];
    int   wav_in_tmp = 0;       /* said in the progress line */
    /* ROOM FOR EVERY ADVANCED OPTION: seven flags with values, a
     * stacked font, the two positionals and the terminator. */
    char *argv[32];
    char  v_voices[12], v_rate[12], v_gain[16];
    char  v_song[VLHE_PATH_MAX + 8];
    char  v_gm[VLHE_PATH_MAX], v_songfollow[VLHE_PATH_MAX];
                                /* Midi Settings' fonts when the
                                 * General MIDI menu follows them */
    int   nf, n, rc, mp3;
    unsigned long est_frames = 0;
    char  errline[256];
    char  kept[64];             /* LAME's stderr capture, on failure */

    (void)w;
    (void)data;

    /* UNSAVED MIDI OR RENDER SETTINGS? The shell asks and may commit
     * them first; a 0 means the user cancelled. Before anything is
     * read from the widgets, so a save here is seen below. */
    if (g_before_cb != NULL && !g_before_cb())
        return;

    in  = gtk_entry_get_text(GTK_ENTRY(g_in));
    out = gtk_entry_get_text(GTK_ENTRY(g_out));
    mp3 = g_mp3 != NULL && GTK_TOGGLE_BUTTON(g_mp3)->active;
    strncpy(in_buf, in != NULL ? in : "", sizeof in_buf - 1);
    in_buf[sizeof in_buf - 1] = '\0';
    strncpy(out_buf, out != NULL ? out : "", sizeof out_buf - 1);
    out_buf[sizeof out_buf - 1] = '\0';
    in = in_buf;
    out = out_buf;

    if (in == NULL || *in == '\0') {
        say(STR_RND_MSG_PICK_MIDI_FILE_FIRST);
        return;
    }
    if (out == NULL || *out == '\0') {
        say(STR_RND_MSG_GIVE_OUTPUT_NAME);
        return;
    }

    /*
     * THE FONT IS THE MIDI PAGE'S SETTING, AND IT IS READ FROM THE
     * SAVED CONFIG rather than from that page's widgets.
     *
     * THAT IS DELIBERATE AND THE USER CONFIRMED IT, 2026-09-22: "I
     * think reading from the config is right ... If you change it and
     * dont apply it then I think thats on the user." One page owns
     * the setting; a render reaching across into another page's
     * unsaved widgets would be a second route to the same value and
     * the two would disagree.
     *
     * BUT IT IS INCONSISTENT WITH EVERYTHING ELSE ON THIS PAGE, which
     * IS read live - the Advanced tab's voices, rate and gain all
     * come from the widgets, so an unapplied change there DOES take
     * effect and an unapplied font does not. That is a real trap and
     * the message is what has to close it: "see Midi Settings" sent
     * the user to a page where the font already looked set, because
     * they had chosen it and not applied it.
     *
     * SO THE MESSAGE SAYS APPLY. The user's request, same day.
     */
    nf = vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL);
    if (nf <= 0 || fonts[0].path[0] == '\0') {
        say(STR_RND_MSG_NO_SOUNDFONT_SET_CHOOSE);
        return;
    }
    /* A SONG FONT ALONE IS NO FONT - 2026-10-06, the documentation
     * review's finding 1. The list is packed, so a lone bank-1 font
     * arrives as fonts[0] and passed the test above, and the renderer
     * then took `FILE@1' as its base font and failed. */
    if (fonts[0].bank != 0) {
        say(STR_RND_MSG_SONG_FONT_NO_GM);
        return;
    }

    /* WHERE THE WAV GOES. Straight to the output when that is what
     * was asked for; to a temporary beside it when LAME will take it
     * from there. Beside it rather than in /tmp, because a 40 MB
     * render on a machine with a small /tmp is a real failure and
     * the output directory is one the user has already chosen. */
    if (mp3) {
        lame = gtk_entry_get_text(GTK_ENTRY(g_lame));
        strncpy(lame_buf, lame != NULL ? lame : "", sizeof lame_buf - 1);
        lame_buf[sizeof lame_buf - 1] = '\0';
        lame = lame_buf;
        if (*lame == '\0') {
            say(STR_RND_MSG_SAY_WHERE_LAME_TURN);
            return;
        }
        if (access(lame, X_OK) != 0) {
            say(STR_RND_MSG_LAME_PATH_NOT_EXECUTABLE);
            return;
        }
        /*
         * WHERE THE TEMPORARY WAV GOES. Beside the output was the
         * only place until 2026-10-02, and on a small transfer volume
         * the WAV - ten times the MP3 - filled it before LAME ran.
         * Automatic takes whichever of /tmp and the output's folder
         * has more room; the menu can force either.
         *
         * A /tmp NAME IS MADE WITH vlhe_tmp_open() - a fresh private
         * file, never a predictable name that may already be there,
         * since this runs as root on these machines (design/48 S6).
         * The descriptor is closed at once; smf2wav opens the path
         * itself. The name has no .wav on it and smf2wav does not
         * mind - it reads nothing from the name.
         */
        {
            int where = g_wavtemp != NULL ? choice_index(g_wavtemp)
                                          : VLHE_WAVTEMP_AUTO;

            default_output(out, ".render.wav", wav, sizeof wav);
            if (where == VLHE_WAVTEMP_AUTO
                && free_bytes("/tmp/") > free_bytes(wav))
                where = VLHE_WAVTEMP_TMP;
            if (where == VLHE_WAVTEMP_TMP) {
                int fd = vlhe_tmp_open(wav, sizeof wav, "vlhe-render");

                if (fd >= 0) {
                    close(fd);
                    wav_in_tmp = 1;
                } else {
                    /* /tmp would not take a file - beside the output
                     * after all, which is where it always went. */
                    default_output(out, ".render.wav", wav, sizeof wav);
                }
            }
        }
    } else {
        lame = NULL;
        strncpy(wav, out, sizeof wav - 1);
        wav[sizeof wav - 1] = '\0';
    }

    say(STR_RND_MSG_RENDERING);
    while (gtk_events_pending())
        gtk_main_iteration();   /* so the message is drawn first */

    n = 0;
    argv[n++] = (char *) smf2wav_path();

    /*
     * THE ADVANCED TAB, OR NOT. `Use VLHE settings' ticked means
     * render it the way the machine plays it: the fonts Midi
     * Settings holds, and smf2wav's own defaults for everything
     * else, which ARE the researched answers from design/20 and 21.
     *
     * Unticked, every control on that tab is passed explicitly - so
     * a comparison render states its settings rather than inheriting
     * them, which is what those documents require of any comparison.
     */
    if (g_use_vlhe != NULL && !GTK_TOGGLE_BUTTON(g_use_vlhe)->active) {
        static const char *const laws[] = { "spec", "linear" };
        static const char *const vfs[]  = { "awe", "2.01", "2.04",
                                            "none" };
        const char *gmf, *songf;
        int i;

        gmf   = font_chosen(g_gm, 1);
        songf = font_chosen(g_song, 1);
        if (gmf == NULL) {
            /* THE FOLLOW ENTRY: Midi Settings' fonts, both of them
             * unless a song font was chosen here. With Midi Settings
             * on (none) there is nothing to follow and the render is
             * refused - it used to go ahead with whichever font
             * sorted first (2026-10-02, see font_menu_fill()). */
            struct vlhe_font cfg[VLHE_MAX_FONTS];
            int ncfg = vlhe_fonts_effective(cfg, VLHE_MAX_FONTS, NULL);

            if (ncfg > 0 && cfg[0].path[0] != '\0') {
                strcpy(v_gm, cfg[0].path);
                gmf = v_gm;
                if (songf == NULL && ncfg > 1 && cfg[1].path[0] != '\0') {
                    strcpy(v_songfollow, cfg[1].path);
                    songf = v_songfollow;
                }
            }
        }
        if (gmf == NULL) {
            say(STR_RND_MSG_NO_GENERAL_MIDI_FONT);
            if (wav_in_tmp)
                unlink(wav);    /* the empty /tmp file we made */
            return;
        }

        sprintf(v_voices, "%d",
                gtk_spin_button_get_value_as_int(
                    GTK_SPIN_BUTTON(g_voices)));
        argv[n++] = "-p";
        argv[n++] = v_voices;

        sprintf(v_rate, "%d", rate_at(choice_index(g_rate)));
        argv[n++] = "-r";
        argv[n++] = v_rate;

        sprintf(v_gain, "%.2f",
                gtk_spin_button_get_value_as_float(
                    GTK_SPIN_BUTTON(g_gain)));
        argv[n++] = "-g";
        argv[n++] = v_gain;

        i = choice_index(g_law);
        argv[n++] = "-L";
        argv[n++] = (char *) laws[i >= 0 && i < 2 ? i : 0];

        i = choice_index(g_vfilter);
        argv[n++] = "-F";
        argv[n++] = (char *) vfs[i >= 0 && i < 4 ? i : 0];

        argv[n++] = "-E";
        argv[n++] = (char *) VLHE_FX_WORD(GTK_TOGGLE_BUTTON(g_reverb)->active,
                                          GTK_TOGGLE_BUTTON(g_chorus)->active);

        argv[n++] = "-B";
        argv[n++] = GTK_TOGGLE_BUTTON(g_bypass)->active ? "on" : "off";

        argv[n++] = "-M";
        argv[n++] = choice_index(g_modenv) == 1 ? "reference" : "fast";

        if (songf != NULL) {
            /* BANK 1, the idiom render.h:493 records from the corpus:
             * a GM font in bank 0 and the song's own in bank 1. */
            sprintf(v_song, "%.200s@1", songf);
            argv[n++] = "-s";
            argv[n++] = v_song;
        }
        argv[n++] = (char *) gmf;
    } else {
        /*
         * THE MACHINE'S SYNTH SETTINGS, NOT ONLY ITS FONTS - design/47
         * R5, the user 2026-10-01: "Use VLHE settings should be the
         * same as the Midi settings. Nothing from the Advanced Tab."
         * This passed the fonts and let vmidid's compiled-in defaults
         * decide voices, gain, law, filter, effects and rate, so a
         * render "the way the machine plays it" could differ from the
         * machine. The same spelling the plan uses for the daemon's
         * line (vlhe_apply.c, the vmidid step), from the same
         * vlhe_synth(); the rate only when it is not the default, as
         * the plan does.
         */
        struct vlhe_synth sy;
        int i;

        if (vlhe_synth(&sy) == 0) {
            static const char *const vfs2[] = { "awe", "2.01", "2.04",
                                                "none" };

            sprintf(v_voices, "%d", sy.voices);
            argv[n++] = "-p";
            argv[n++] = v_voices;
            sprintf(v_gain, "%d.%03d", sy.gain_milli / 1000,
                    sy.gain_milli % 1000);
            argv[n++] = "-g";
            argv[n++] = v_gain;
            argv[n++] = "-L";
            argv[n++] = sy.law == VLHE_LAW_LINEAR ? "linear" : "spec";
            if (sy.filter >= 0 && sy.filter <= VLHE_VF_NONE) {
                argv[n++] = "-F";
                argv[n++] = (char *) vfs2[sy.filter];
            }
            argv[n++] = "-E";
            argv[n++] = (char *) VLHE_FX_WORD(sy.reverb, sy.chorus);
            /* THE MACHINE'S MODE TOO - design/54 D28. */
            argv[n++] = "-M";
            argv[n++] = sy.modenv == VLHE_MODENV_FAST ? "fast" : "reference";
            if (sy.rate > 0 && sy.rate != 44100) {
                sprintf(v_rate, "%d", sy.rate);
                argv[n++] = "-r";
                argv[n++] = v_rate;
            }
        }

        /* STACKED FONTS, the same -s form vmidid takes: the FIRST
         * font is positional and the rest are -s, so a two-font
         * setup renders exactly as it plays. */
        for (i = 1; i < nf && n < 20; i++) {
            if (fonts[i].path[0] == '\0')
                continue;
            argv[n++] = "-s";
            argv[n++] = fonts[i].path;
        }
        argv[n++] = fonts[0].path;
    }

    argv[n++] = (char *) in;
    argv[n++] = wav;
    argv[n]   = NULL;

    /*
     * THE PRE-FLIGHT CHECK. Both numbers come from outside this
     * function - the estimate from smf2wav, the free space from
     * statfs - and either failing to answer means no warning rather
     * than no render.
     *
     * THE MP3 CASE WARNS ON THE WAV, which is the bigger of the two:
     * the temporary is full-size and lives until LAME has finished
     * with it, so a machine that cannot hold the WAV cannot make the
     * MP3 either, however small the MP3 would be.
     */
    {
        unsigned long need = 0, have, secs = 0;
        int capped = 0;

        est_frames = render_estimate(argv, &need, &secs, &capped);

        /* LONG FIRST: the length is the question; space follows it. */
        if (secs > RENDER_ASK_SECONDS && !confirm_long(secs, need, capped)) {
            say(STR_RND_MSG_CANCELLED);
            if (wav_in_tmp)
                unlink(wav);        /* the empty /tmp file we made */
            return;
        }
        have = free_bytes(wav);

        if (need > 0 && have > 0 && need > have) {
            if (!confirm_space(need, have)) {
                say(STR_RND_MSG_CANCELLED);
                if (wav_in_tmp)
                    unlink(wav);    /* the empty /tmp file we made */
                return;
            }
        }
    }

    rc = run_wait_progress(argv, est_frames,
                           wav_in_tmp ? STR_RND_TEXT_RENDERING_WAV_IN_TMP
                                      : STR_RND_TEXT_RENDERING,
                           errline, sizeof errline, NULL, 0);
    if (rc == RUN_CANCELLED) {
        /* THE PARTIAL FILE GOES. smf2wav was stopped mid-write, so
         * what is there has a RIFF header claiming a size it never
         * reached - in WAV mode that is the user's named output, in
         * MP3 mode the temporary. Either way it is ours, half-made,
         * and asked for by nobody now. */
        unlink(wav);
        say(STR_RND_MSG_CANCELLED_PARTIAL_FILE_WAS);
        return;
    }
    if (rc != 0) {
        /*
         * WHAT smf2wav SAID, WHEN IT SAID ANYTHING.
         *
         * THE MESSAGE USED TO GUESS. It read "the render failed - out
         * of disk space, or not a MIDI file", naming two unrelated
         * causes because every exit but 2 and 127 mapped to one
         * string - and the user met it on a real out-of-space render,
         * 2026-09-22. smf2wav had said exactly which it was, on a
         * stderr that went to /dev/null.
         *
         * 127 STAYS A SPECIAL CASE because it comes from the failed
         * execvp in our own child, so there is no smf2wav to have
         * said anything. The ceiling keeps its wording too - exit 2
         * is unambiguous and the message can be kinder than the
         * diagnostic.
         */
        if (rc == 127)
            say(STR_RND_MSG_SMF2WAV_NOT_PATH);
        else if (rc == 2)
            say(STR_RND_MSG_STOPPED_20_MINUTE_CEILING);
        else if (errline[0] != '\0') {
            char msg[256];

            /* ITS OWN WORDS, PREFIXED SO THE SOURCE IS CLEAR. The
             * line already begins "smf2wav: " when it comes from the
             * renderer's own diagnostics, so this adds nothing when
             * it would read twice. */
            if (strncmp(errline, "smf2wav", 7) == 0)
                sprintf(msg, "%.200s", errline);
            else
                sprintf(msg, FMT_RND_RENDER_FAILED, errline);
            say(msg);
        } else
            say(STR_RND_MSG_RENDER_FAILED_SAID_NOTHING);

        /*
         * THE PARTIAL FILE GOES, AS IT DOES ON CANCEL - 2026-10-04. A
         * failed render left it: smf2wav had written a header of zero
         * lengths (it rewrites the real ones only when every write
         * succeeded), so nothing lied about its size, but a dead file
         * sat where the user asked for one. EXCEPT AT THE CEILING (exit
         * 2) WHEN IT IS THE USER'S WAV: smf2wav then wrote a correct
         * header for what it rendered, so the file is valid, only
         * shorter, and the message says so. In MP3 mode the WAV is ours,
         * an intermediate, and goes either way.
         */
        if (rc != 2 || mp3)
            unlink(wav);
        return;
    }

    if (!mp3) {
        say(STR_RND_MSG_DONE);
        return;
    }

    say(STR_RND_MSG_ENCODING);
    while (gtk_events_pending())
        gtk_main_iteration();

    n = 0;
    argv[n++] = (char *) lame;
    /*
     * `--disptime 1' RATHER THAN `--quiet' - the user asked for the
     * encode to show progress the way the render does, 2026-09-22.
     *
     * LAME PRINTS A TABLE with a frame count, a percentage and an
     * ETA, and it is the right source: it knows the total frames,
     * where we would only be guessing from the WAV's size. The catch
     * is that it goes to STDERR, separated by CARRIAGE RETURNS - it
     * is drawing over one line - which is why run_wait_progress
     * polls the captured file rather than reading a pipe.
     *
     * `-S' WOULD SUPPRESS EXACTLY THIS and --quiet suppresses
     * everything, which is what it did until now.
     */
    argv[n++] = "--disptime";
    argv[n++] = "1";
    argv[n++] = (char *) wav;
    argv[n++] = (char *) out;
    argv[n]   = NULL;

    /* WHAT `out' WAS BEFORE LAME TOUCHED IT - see the failure branch. */
    had_out = stat(out, &before) == 0;

    rc = run_wait_progress(argv, 0, STR_RND_TEXT_ENCODING, errline, sizeof errline,
                           kept, sizeof kept);

    /* THE TEMPORARY GOES EITHER WAY. It is ours, we made it in this
     * function, and leaving a 40 MB .render.wav behind after a failed
     * encode is worse than the failure. */
    unlink(wav);

    /* LAME'S OWN COMPLAINT TOO, for the same reason - "check the path
     * and the file" is the guess this change exists to stop making. */
    if (rc == RUN_CANCELLED) {
        /* AND THE PARTIAL MP3 - LAME was stopped mid-file. */
        unlink(out);
        say(STR_RND_MSG_CANCELLED_PARTIAL_FILE_WAS);
        return;
    }
    if (rc == 0) {
        say(STR_RND_MSG_DONE);
        return;
    }

    /* THE PARTIAL MP3 GOES ON FAILURE TOO - the user, 2026-10-02,
     * having filled the transfer volume: LAME stops with "Error
     * writing mp3 output" and what it wrote so far stayed, so the
     * disk was as full after the failure as during it.
     *
     * ONLY IF LAME WROTE IT. A LAME that failed before opening its
     * output - a bad path to itself (exit 127, our execvp), an input
     * it would not read - leaves `out' as the user's old file, and
     * removing THAT would turn a failed encode into a lost one. So:
     * gone if it was not there before, or if its size or time moved
     * while LAME ran. */
    {
        struct stat after;

        if (!had_out
            || stat(out, &after) != 0
            || after.st_size  != before.st_size
            || after.st_mtime != before.st_mtime)
            unlink(out);
    }
    if (rc == 127)
        say(STR_RND_MSG_LAME_FAILED_NOT_PATH);
    else if (kept[0] != '\0') {
        char msg[160];

        /* THE PATH IN THE STATUS LINE, THE WHOLE OUTPUT IN A WINDOW -
         * the user, 2026-10-02: "Show the path and a window with the
         * error. We already do similar things for simulate load and
         * unload." Nothing of LAME's is parsed any more. */
        sprintf(msg, FMT_RND_LAME_FAILED_EXIT_OUTPUT,
                rc, kept);
        say(msg);
        lame_failed(rc, kept);
    } else
        say(STR_RND_MSG_LAME_FAILED_CHECK_PATH);
}

/*
 * LAME'S OUTPUT, WHOLE, IN A WINDOW - on the Status page's result box
 * (vlhe_mod_status.c show_result()): a modal dialog, a text widget in
 * a scrolled window, Close. The user asked for it after two attempts
 * to pick LAME's error out of its progress table failed, the second
 * because LAME writes the carriage return at the START of each row,
 * so the last row and "Error writing mp3 output" are one segment
 * with nothing between them. Any rule for that is a rule about one
 * LAME's output; showing all of it is a rule about none.
 *
 * ONE DISPLAY CHOICE ONLY: a carriage return becomes a line break, so
 * the rows LAME drew over each other read as a list rather than as
 * one line a thousand characters wide. That is display, not parsing -
 * nothing is dropped.
 *
 * THE FILE IS LEFT WHERE IT IS, and the last line says so, so a user
 * who wants to copy it out or attach it to a report can. One small
 * private file per failed encode.
 */
static void
lame_failed(int rc, const char *path)
{
    GtkWidget *dlg, *sw, *text, *btn;
    FILE      *fp;
    char       tail[160];

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_RND_TITLE_LAME_FAILED);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    gtk_widget_set_usize(dlg, 480, 300);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), sw,
                       TRUE, TRUE, 4);
    gtk_widget_show(sw);

    text = gtk_text_new(NULL, NULL);
    gtk_text_set_editable(GTK_TEXT(text), FALSE);
    gtk_container_add(GTK_CONTAINER(sw), text);
    gtk_widget_show(text);

    gtk_text_freeze(GTK_TEXT(text));

    fp = fopen(path, "r");
    if (fp != NULL) {
        int c;
        char ch[2];

        ch[1] = '\0';
        while ((c = fgetc(fp)) != EOF) {
            ch[0] = (c == '\r') ? '\n' : (char) c;
            gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, ch, 1);
        }
        fclose(fp);
    } else {
        gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL,
                        STR_RND_TEXT_CAPTURE_COULD_NOT_READ, -1);
    }

    sprintf(tail, FMT_RND_LAME_EXITED_STATUS_OUTPUT, rc, path);
    gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, tail, -1);

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

/* ------------------------------------------------------------------ */

static GtkWidget *
labelled(GtkWidget *vbox, const char *text, GtkWidget *control,
         GtkWidget *after)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *lab  = gtk_label_new(text);

    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
    gtk_widget_set_usize(lab, 110, -1);
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    gtk_box_pack_start(GTK_BOX(hbox), control, TRUE, TRUE, 0);
    gtk_widget_show(control);

    if (after != NULL) {
        gtk_box_pack_start(GTK_BOX(hbox), after, FALSE, FALSE, 0);
        gtk_widget_show(after);
    }

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);
    return hbox;
}

/* ------------------------------------------------------------------ */
/* The Advanced tab                                                   */
/* ------------------------------------------------------------------ */

/*
 * THE SWITCH NO LONGER GREYS THE ADVANCED TAB - the user's call,
 * 2026-09-22: "the check box doesnt need to disable the advanced tab
 * controls but when we hit render it needs to know to use which
 * settings to use".
 *
 * WHICH IS THE BETTER ARRANGEMENT. Those controls are SAVED SETTINGS
 * in their own right now, with their own OK/Apply/Cancel, so a user
 * setting up a comparison render should be able to edit and save
 * them without first unticking something on another tab. The switch
 * says which set the NEXT RENDER uses; it does not own the controls.
 *
 * Kept as a handler rather than removed, because the status line is
 * worth having - the two states are easy to lose track of when the
 * tab that shows them is not the tab you are on.
 */
static void
on_use_vlhe_toggled(GtkWidget *w, gpointer d)
{
    (void)d;
    /* NOT WHILE THE PAGE IS BEING SET - design/47 Q6. render_reload()
     * sets this toggle during the build, before the shell has a status
     * bar, and the say() landed in a Gtk-CRITICAL at startup on any
     * machine with UseVlheSettings=0 saved. A set is not a click. */
    if (!g_loading)
        say(GTK_TOGGLE_BUTTON(w)->active
            ? STR_RND_MSG_RENDERS_USE_MIDI_SETTINGS
            : STR_RND_MSG_RENDERS_USE_ADVANCED);
    font_label_refresh();
}

/*
 * A dropdown of the fonts the MIDI page found. `none_label' is the
 * FIRST ENTRY, meaning "no font of my own" - "(none)" for the song
 * slot, GM_FOLLOW_LABEL for the General MIDI slot - or NULL for no
 * such entry (no caller passes NULL today; the parameter is kept so
 * the menu shape is explicit at every call).
 *
 * THE GENERAL MIDI MENU HAD NO SUCH ENTRY UNTIL 2026-10-02, and the
 * user caught what that did: "On the renderer it automatically
 * selects the first font. Midi Settings stays (none)". With nothing
 * saved and Midi Settings on (none), the menu could only show SOME
 * font, so it showed whichever sorted first - as a choice nobody had
 * made, and a render with Use VLHE off would have used it. Midi
 * Settings' own rule (vlhe_mod_midi.c, "(none) IS ALWAYS FIRST") is
 * that a menu never picks for the user; now this one follows it. The
 * first entry is R4's follow-the-Midi-Settings rule made visible: an
 * empty saved GmFont already meant that, and now it looks like it.
 */
#define GM_FOLLOW_LABEL STR_RND_TEXT_GM_FOLLOW
#define SONG_NONE_LABEL STR_RND_TEXT_SONG_NONE

static void
font_menu_fill(GtkWidget *opt, const char *none_label, const char *current,
               int *sel_out)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;
    int i, n = 0, sel = 0;
    int allow_none = none_label != NULL;

    if (allow_none) {
        item = gtk_menu_item_new_with_label(none_label);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        n++;
    }

    for (i = 0; i < g_navail; i++) {
        const char *base = strrchr(g_avail[i], '/');

        item = gtk_menu_item_new_with_label(base ? base + 1 : g_avail[i]);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        if (current != NULL && strcmp(g_avail[i], current) == 0)
            sel = n;
        n++;
    }

    if (g_navail == 0) {
        /* THE LIST, not the menu - with a first entry the menu is
         * never empty, and this line is what tells the user why
         * there is nothing else to pick. */
        item = gtk_menu_item_new_with_label(STR_RND_MENU_NO_SOUNDFONTS_FOUND);
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
    }

    gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(opt), sel);
    /*
     * THE HANDLER LIVES ON THE GtkMenu, AND THIS IS A NEW GtkMenu -
     * 2026-09-24, the user: "then it stops updating". build_advanced()
     * connected "deactivate" -> mark_dirty on the menu it built; the
     * first refill replaced that menu and the connection went with
     * it, so after one page-show nothing marked the page dirty or
     * refreshed the label. Connected HERE, so every fill has it.
     */
    gtk_signal_connect(GTK_OBJECT(menu), "deactivate",
                       GTK_SIGNAL_FUNC(on_font_menu_changed), NULL);
    if (sel_out != NULL)
        *sel_out = sel;
}

static GtkWidget *
font_menu(const char *none_label, const char *current, int *sel_out)
{
    GtkWidget *opt = gtk_option_menu_new();

    font_menu_fill(opt, none_label, current, sel_out);
    return opt;
}

/* Which font a dropdown is on, or NULL for "(none)" / empty. */
static const char *
font_chosen(GtkWidget *opt, int allow_none)
{
    int i;

    if (opt == NULL || g_navail == 0)
        return NULL;
    i = GTK_OPTION_MENU(opt)->menu_item != NULL
        ? g_list_index(GTK_MENU_SHELL(GTK_OPTION_MENU(opt)->menu)->children,
                       GTK_OPTION_MENU(opt)->menu_item)
        : 0;
    if (allow_none) {
        if (i <= 0)
            return NULL;
        i--;
    }
    if (i < 0 || i >= g_navail)
        return NULL;
    return g_avail[i];
}

/* A labelled option menu built from a NULL-terminated string list. */
static GtkWidget *
choice_menu(const char *const *items, int current)
{
    GtkWidget *opt = gtk_option_menu_new();
    GtkWidget *menu = gtk_menu_new();
    int i;

    for (i = 0; items[i] != NULL; i++) {
        GtkWidget *it = gtk_menu_item_new_with_label(items[i]);
        gtk_menu_append(GTK_MENU(menu), it);
        gtk_widget_show(it);
    }
    gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(opt), current);
    return opt;
}

static int
choice_index(GtkWidget *opt)
{
    if (opt == NULL || GTK_OPTION_MENU(opt)->menu_item == NULL)
        return 0;
    return g_list_index(
        GTK_MENU_SHELL(GTK_OPTION_MENU(opt)->menu)->children,
        GTK_OPTION_MENU(opt)->menu_item);
}

static GtkWidget *
build_advanced(void)
{
    static const int   rate_hz[] = VLHE_SYNTH_RATES;
    static char        rate_txt[VLHE_SYNTH_NRATES][8];
    static const char *rates[VLHE_SYNTH_NRATES + 1];
    static const char *const laws[] = {
        STR_RND_LAW_SPEC,
        STR_RND_LAW_LINEAR,
        NULL
    };
    static const char *const vfs[] = {
        STR_RND_VF_AWE,
        STR_RND_VF_201,
        STR_RND_VF_204,
        STR_RND_VF_NONE,
        NULL
    };
    /* ORDER IS THE VALUE THE PAGE READS: 0 fast, 1 spec (design/54 D70). */
    static const char *const modenvs[] = {
        STR_MID_MENU_MODENV_FAST,
        STR_MID_MENU_MODENV_SPEC,
        NULL
    };
    struct vlhe_synth sy;
    GtkWidget *outer, *vbox, *note;
    GtkObject *adj;
    int rate_sel = 0, i;

    outer = gtk_vbox_new(FALSE, 0);
    gtk_container_border_width(GTK_CONTAINER(outer), 8);

    /* THE `Use VLHE settings' SWITCH IS ON THE RENDER TAB, not here -
     * the user's call, 2026-09-22. It belongs where the decision is
     * made: someone who never opens this tab still needs to see that
     * a choice exists, and someone who does open it has already
     * chosen. This tab is only what the switch turns on. */
    g_adv_box = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(g_adv_box), 8);
    gtk_box_pack_start(GTK_BOX(outer), g_adv_box, FALSE, FALSE, 0);
    gtk_widget_show(g_adv_box);

    vbox = g_adv_box;

    /* THE FONTS, from the MIDI page's list, both menus on their first
     * entry: the General MIDI one FOLLOWS Midi Settings until a font
     * is chosen here, the song one is "(none)". render_reload() sets
     * what was saved. This used to seed both with Midi Settings'
     * fonts as if chosen - see font_menu_fill(). */
    g_navail = vlhe_available_fonts(g_avail, VLHE_MAX_AVAIL);

    g_gm = font_menu(GM_FOLLOW_LABEL, NULL, NULL);
    labelled(vbox, STR_RND_LABEL_GENERAL_MIDI, g_gm, NULL);

    g_song = font_menu(SONG_NONE_LABEL, NULL, NULL);
    labelled(vbox, STR_RND_LABEL_SONG_FONT, g_song, NULL);

    /* VOICES - smf2wav's offline range, not the daemon's 64. */
    adj = gtk_adjustment_new((float) RENDER_VOICES_DEF, 1.0,
                             (float) RENDER_VOICES_MAX, 1.0, 16.0, 0.0);
    g_voices = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 1.0, 0);
    labelled(vbox, STR_RND_LABEL_VOICES, g_voices,
             gtk_label_new(STR_RND_LABEL_1_2048_OFFLINE_HAS));

    /* THE SAME FOUR AS MIDI SETTINGS (vlhe_rates.h), offered as plain
     * numbers beside the "Hz" label - and the Midi Settings rate as
     * the starting choice. */
    for (i = 0; i < VLHE_SYNTH_NRATES; i++) {
        sprintf(rate_txt[i], "%d", rate_hz[i]);
        rates[i] = rate_txt[i];
    }
    rates[VLHE_SYNTH_NRATES] = NULL;
    if (vlhe_synth(&sy) == 0)
        rate_sel = rate_index(sy.rate);
    g_rate = choice_menu(rates, rate_sel);
    labelled(vbox, STR_RND_LABEL_SAMPLE_RATE, g_rate, gtk_label_new(STR_RND_LABEL_HZ));

    adj = gtk_adjustment_new(0.5, 0.01, 2.0, 0.05, 0.1, 0.0);
    g_gain = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 0.05, 2);
    labelled(vbox, STR_RND_LABEL_MASTER_GAIN, g_gain,
             gtk_label_new(STR_RND_LABEL_1_0_UNITY_2));

    g_law = choice_menu(laws, VLHE_LAW_SPEC);
    labelled(vbox, STR_RND_LABEL_VOLUME_LAW, g_law, NULL);

    g_vfilter = choice_menu(vfs, VLHE_VF_AWE);
    labelled(vbox, STR_RND_LABEL_VELOCITY_FILTER, g_vfilter, NULL);

    /* REVERB AND CHORUS, ONE ROW - as on Midi Settings (2026-10-05). */
    {
        GtkWidget *fxrow = gtk_hbox_new(FALSE, 16);

        g_reverb = vlhe_tipped(gtk_check_button_new_with_label(
            STR_MID_CHECK_REVERB), STR_MID_CHECK_REVERB_TIP);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_reverb), TRUE);
        gtk_box_pack_start(GTK_BOX(fxrow), g_reverb, FALSE, FALSE, 0);
        gtk_widget_show(g_reverb);

        g_chorus = vlhe_tipped(gtk_check_button_new_with_label(
            STR_MID_CHECK_CHORUS), STR_MID_CHECK_CHORUS_TIP);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_chorus), TRUE);
        gtk_box_pack_start(GTK_BOX(fxrow), g_chorus, FALSE, FALSE, 0);
        gtk_widget_show(g_chorus);

        gtk_box_pack_start(GTK_BOX(vbox), fxrow, FALSE, FALSE, 0);
        gtk_widget_show(fxrow);
    }

    /*
     * NAMED AND THEN JUSTIFIED - the user, 2026-09-22, in two passes.
     * First: "if it does nothing then why would we include it. Our
     * wording is the issue." Then, when a version dropped the name
     * altogether: "It should still say Low Pass Filter - Speeds up
     * Rendering, difference should not be audible."
     *
     * Both halves are needed. "Bypass the low-pass filter where it
     * would do nothing" said WHAT and gave no reason to want it;
     * "Speed up rendering" gave the reason and lost WHICH control
     * this is, which matters on a tab of seven. It is a SPEED
     * option: the filter is skipped
     * only for voices whose cutoff sits above anything their
     * resampled sample can carry, where the biquad could only add
     * phase shift.
     *
     * THE NUMBERS, from design/20 section 3 on the workstation: 94 %
     * of frames filtered down to 63 %, render time down 14 %, worst
     * per-block level change 0.17 dB - well under the ~1 dB anyone
     * can hear. Hence "should be inaudible" rather than "is": it has
     * NOT been measured on the target, which is why the switch
     * exists at all rather than the rule being on always.
     *
     * THE DETAIL IS IN THE HELP FILE (Render MIDI / Advanced).
     *
     * THE LABEL SAYS "DISABLE" SINCE 2026-10-06 (design/54 G19). It
     * read "Low Pass Filter - speeds up rendering", which reads as
     * turning the filter ON - while ticking it sends `-B on', the
     * BYPASS. The mapping was right; the words were not. The wording
     * is the user's: "Disable Low Pass Filter - speeds up rendering,
     * difference should not be audible" (after a first fix said
     * "Skip ... where it changes nothing").
     */
    g_bypass = vlhe_tipped(gtk_check_button_new_with_label(
        STR_RND_CHECK_LOW_PASS_FILTER_SPEEDS), STR_RND_CHECK_LOW_PASS_FILTER_SPEEDS_TIP);
    gtk_box_pack_start(GTK_BOX(vbox), g_bypass, FALSE, FALSE, 0);
    gtk_widget_show(g_bypass);

    /* THE MODULATION ENVELOPE'S MODE - design/54 D28, a drop-down
     * since D70: index 0 Fast (the default), 1 To the SoundFont spec.
     * [Render Settings] ModEnv. */
    g_modenv = vlhe_tipped(choice_menu(modenvs, 0), STR_RND_MENU_MODENV_TIP);
    labelled(vbox, STR_RND_LABEL_MODENV, g_modenv, NULL);

    /* EVERY CONTROL MARKS THE PAGE DIRTY, so the sidebar's asterisk
     * and the unsaved-changes prompt work as they do everywhere
     * else. The option menus signal on their MENU, not themselves. */
    gtk_signal_connect(GTK_OBJECT(g_voices), "changed",
                       GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(GTK_OBJECT(g_gain), "changed",
                       GTK_SIGNAL_FUNC(mark_dirty), NULL);
    /* THE LAME PATH TOO - design/47 R6: it is saved by gather() and
     * was the one field here that did not mark the page. */
    if (g_lame != NULL)
        gtk_signal_connect(GTK_OBJECT(g_lame), "changed",
                           GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(GTK_OBJECT(g_reverb), "toggled",
                       GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(GTK_OBJECT(g_chorus), "toggled",
                       GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(GTK_OBJECT(g_bypass), "toggled",
                       GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(
        GTK_OBJECT(GTK_OPTION_MENU(g_modenv)->menu), "deactivate",
        GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(
        GTK_OBJECT(GTK_OPTION_MENU(g_rate)->menu), "deactivate",
        GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(
        GTK_OBJECT(GTK_OPTION_MENU(g_law)->menu), "deactivate",
        GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_signal_connect(
        GTK_OBJECT(GTK_OPTION_MENU(g_vfilter)->menu), "deactivate",
        GTK_SIGNAL_FUNC(mark_dirty), NULL);
    /* g_gm and g_song: font_menu_fill() connects their handler, because
     * it is the one that knows when the menu has been replaced. */

    /* THE USER'S OWN WORDS, 2026-09-22, replacing three paragraphs
     * of mine. Theirs says what a user needs to know; mine explained
     * what I had just built. */
    note = gtk_label_new(
        STR_RND_LABEL_THESE_ARE_SETTINGS_RENDERING);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped by GTK at 580, the width
     * confirmed on the target. Hand breaks made it look fixed. */
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_usize(note, 580, -1);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 8);
    gtk_widget_show(note);

    gtk_widget_show(outer);
    return outer;
}

static GtkWidget *
build_basic(void)
{
    GtkWidget *outer, *frame, *vbox, *browse, *note;
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    int nf;

    outer = gtk_vbox_new(FALSE, 0);
    gtk_container_border_width(GTK_CONTAINER(outer), 8);

    frame = gtk_frame_new(STR_RND_FRAME_RENDER_MIDI_FILE);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);
    gtk_widget_show(frame);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);
    gtk_widget_show(vbox);

    g_in = gtk_entry_new_with_max_length(VLHE_PATH_MAX - 1);
    browse = vlhe_tipped(gtk_button_new_with_label(STR_RND_BTN_BROWSE), STR_RND_BTN_BROWSE_TIP);
    gtk_signal_connect(GTK_OBJECT(browse), "clicked",
                       GTK_SIGNAL_FUNC(on_browse), NULL);
    labelled(vbox, STR_RND_LABEL_MIDI_FILE, g_in, browse);

    g_out = gtk_entry_new_with_max_length(VLHE_PATH_MAX - 1);
    labelled(vbox, STR_RND_LABEL_SAVE_AS, g_out, NULL);

    /* WHICH FONT WILL BE USED, shown rather than chosen - the MIDI
     * page owns the setting and two places to set one thing is one
     * too many. A machine with none set sees why the button will
     * refuse before pressing it. */
    nf = vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL);
    if (nf > 0 && fonts[0].path[0] != '\0') {
        const char *base = strrchr(fonts[0].path, '/');
        char buf[VLHE_PATH_MAX + 32];

        sprintf(buf, "%.200s%s", base != NULL ? base + 1 : fonts[0].path,
                nf > 1 ? STR_RND_TEXT_ONE_STACKED_ABOVE : "");
        g_font = gtk_label_new(buf);
    } else {
        /* SAME WORDING AS THE REFUSAL, and for the same reason - a
         * font chosen and not applied is invisible here, so "see Midi
         * Settings" alone sends someone to a page that looks right. */
        g_font = gtk_label_new(STR_RND_LABEL_NONE_SET_CHOOSE_ONE);
    }
    gtk_misc_set_alignment(GTK_MISC(g_font), 0.0, 0.5);
    labelled(vbox, STR_RND_LABEL_SOUNDFONT, g_font, NULL);

    /*
     * THE SWITCH, ON THIS TAB - the user's call, 2026-09-22, and the
     * right one: it belongs where the decision is made. Someone who
     * never opens Advanced still sees that a choice exists, and the
     * label says which way it is set without them going to look.
     *
     * ON BY DEFAULT means render it the way the machine plays it.
     */
    g_use_vlhe = vlhe_tipped(gtk_check_button_new_with_label(
        STR_RND_CHECK_USE_VLHE_SETTINGS_RENDER), STR_RND_CHECK_USE_VLHE_SETTINGS_RENDER_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_use_vlhe), TRUE);
    gtk_signal_connect(GTK_OBJECT(g_use_vlhe), "toggled",
                       GTK_SIGNAL_FUNC(on_use_vlhe_toggled), NULL);
    gtk_signal_connect(GTK_OBJECT(g_use_vlhe), "toggled",
                       GTK_SIGNAL_FUNC(mark_dirty), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_use_vlhe, FALSE, FALSE, 0);
    gtk_widget_show(g_use_vlhe);

    g_mp3 = vlhe_tipped(gtk_check_button_new_with_label(
                STR_RND_CHECK_ENCODE_MP3_AFTERWARDS_LAME), STR_RND_CHECK_ENCODE_MP3_AFTERWARDS_LAME_TIP);
    gtk_signal_connect(GTK_OBJECT(g_mp3), "toggled",
                       GTK_SIGNAL_FUNC(on_mp3_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_mp3, FALSE, FALSE, 0);
    gtk_widget_show(g_mp3);

    g_lame = gtk_entry_new_with_max_length(VLHE_PATH_MAX - 1);
    g_lame_row = labelled(vbox, STR_RND_LABEL_LAME_IS_AT, g_lame, NULL);
    /* OFF UNTIL THE BOX IS TICKED. LAME is on no Corel disc and is
     * not ours to ship, so the path is empty and the row is greyed
     * until someone asks for MP3 at all. */
    gtk_widget_set_sensitive(g_lame_row, FALSE);

    /*
     * WHERE THE TEMPORARY WAV GOES - the user, 2026-10-02, having
     * filled the transfer volume: "can we do a write to /tmp for the
     * wav and then mp3 gets stored?" then "Lets do both. Automatic
     * by default. and a user can override it". Three choices rather
     * than a box, because a box cannot say "automatic" AND offer
     * either override. Order is VLHE_WAVTEMP_*. Greyed with the LAME
     * row: the WAV is only temporary when there is an MP3 after it.
     */
    {
        static const char *const where[] = {
            STR_RND_WAVTEMP_AUTO,
            STR_RND_WAVTEMP_TMP,
            STR_RND_WAVTEMP_BESIDE,
            NULL
        };
        GtkWidget *row = gtk_hbox_new(FALSE, 0);

        g_wavtemp = choice_menu(where, VLHE_WAVTEMP_AUTO);
        gtk_signal_connect(
            GTK_OBJECT(GTK_OPTION_MENU(g_wavtemp)->menu), "deactivate",
            GTK_SIGNAL_FUNC(mark_dirty), NULL);
        gtk_box_pack_start(GTK_BOX(row), g_wavtemp, FALSE, FALSE, 0);
        gtk_widget_show(g_wavtemp);
        g_wavtemp_row = labelled(vbox, STR_RND_LABEL_TEMPORARY_WAV, row, NULL);
        gtk_widget_set_sensitive(g_wavtemp_row, FALSE);
    }

    g_go = vlhe_tipped(gtk_button_new_with_label(STR_RND_LABEL_RENDER), STR_RND_LABEL_RENDER_TIP);
    gtk_signal_connect(GTK_OBJECT(g_go), "clicked",
                       GTK_SIGNAL_FUNC(on_render), NULL);
    /* NULLED WHEN DESTROYED - R1's other half: closing the window
     * mid-render destroys the page while run_wait_progress() is still
     * pumping, and say() and on_render() then wrote to freed widgets.
     * gtk_widget_destroyed() is GTK's own pointer-clearing handler. */
    gtk_signal_connect(GTK_OBJECT(g_go), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed), &g_go);
    /* CANCEL, AFTER RENDER - the user's order, 2026-10-02: "Have
     * render first and cancel after". Greyed until a render starts.
     *
     * LINED UP WITH THE FIELDS, not the right edge - the user, the
     * same day: "move the render button over so it is lined up ...
     * the start of the other fields". So the row goes through
     * labelled() with an empty label, which puts it exactly where
     * every entry above starts. */
    g_stop = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    gtk_signal_connect(GTK_OBJECT(g_stop), "clicked",
                       GTK_SIGNAL_FUNC(on_cancel), NULL);
    gtk_signal_connect(GTK_OBJECT(g_stop), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed), &g_stop);
    gtk_widget_set_sensitive(g_stop, FALSE);
    {
        GtkWidget *row = gtk_hbox_new(FALSE, 6);
        gtk_box_pack_start(GTK_BOX(row), g_go, FALSE, FALSE, 0);
        gtk_widget_show(g_go);
        gtk_box_pack_start(GTK_BOX(row), g_stop, FALSE, FALSE, 0);
        gtk_widget_show(g_stop);
        vlhe_buttons_equalise(row);
        labelled(vbox, "", row, NULL);
    }

    /* THE PROGRESS BOX - a sunken frame so it reads as a field rather
     * than a caption, holding a label rather than an entry so nothing
     * invites typing into it. say() fills it. */
    {
        GtkWidget *frame = gtk_frame_new(NULL);

        gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
        g_progress = gtk_label_new("");
        gtk_signal_connect(GTK_OBJECT(g_progress), "destroy",
                           GTK_SIGNAL_FUNC(gtk_widget_destroyed),
                           &g_progress);
        gtk_misc_set_alignment(GTK_MISC(g_progress), 0.0, 0.5);
        gtk_misc_set_padding(GTK_MISC(g_progress), 4, 2);
        gtk_container_add(GTK_CONTAINER(frame), g_progress);
        gtk_widget_show(g_progress);
        labelled(vbox, STR_RND_LABEL_PROGRESS, frame, NULL);
    }

    note = gtk_label_new(
        STR_RND_LABEL_RENDERING_OPENS_NO_SOUND);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped by GTK at 580, the width
     * confirmed on the target. Hand breaks made it look fixed. */
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_usize(note, 580, -1);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 8);
    gtk_widget_show(note);

    gtk_widget_show(outer);
    return outer;
}

/*
 * TWO TABS. The first is the whole job for someone who just wants a
 * file; the second is every knob smf2wav has, behind a switch that
 * is ON by default and means "use what Midi Settings holds".
 *
 * THE USER ASKED FOR IT THIS WAY, 2026-09-22: "a separate tab... on
 * the first tab maybe a check box to use \'vlhe settings\' on by
 * default". Putting the seven comparison options on the front page
 * would make a two-field task look like a seven-field one.
 *
 * THE NOTEBOOK IS BUILT HERE, not by the shell. vlhe_cc.c builds one
 * from m->tab[] only for pages that have no builder of their own -
 * this is a MOD_VIEW with a builder, so the tabs are ours.
 */
/* ------------------------------------------------------------------ */
/* The settings trio - the Advanced tab's values                      */
/* ------------------------------------------------------------------ */

/* Set while writing widgets, so the handlers do not mark the page
 * dirty for changes nobody made. The same guard every other module
 * uses. Declared up with the other statics since Q6 made the
 * use-VLHE toggle's handler read it. */
static int g_dirty;
static GtkWidget *g_notebook;

static void (*g_dirty_cb)(void);

void render_set_dirty_cb(void (*cb)(void))
{
    g_dirty_cb = cb;
}

void render_set_before_cb(int (*cb)(void))
{
    g_before_cb = cb;
}

/* THE TAB THE EDIT WAS MADE ON GETS A "*" - the user, 2026-10-01, for
 * every page with tabs ("so a user can tell which tab the setting
 * changed"). The edit comes from the tab on screen; both are cleared
 * when the page is collected or reloaded. */
static int g_tab_dirty[2];

static void tab_mark(int tab, int dirty)
{
    static const char *name[2] = { STR_RND_TAB_RENDER, STR_RND_TAB_ADVANCED };
    GtkWidget *page;
    char lab[16];

    if (g_notebook == NULL || tab < 0 || tab > 1)
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
    if (g_tab_dirty[0])
        tab_mark(0, 0);
    if (g_tab_dirty[1])
        tab_mark(1, 0);
}

static void mark_dirty(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_loading)
        return;
    if (g_notebook != NULL) {
        int t = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));

        if (t >= 0 && t <= 1 && !g_tab_dirty[t])
            tab_mark(t, 1);
    }
    g_dirty = 1;
    /* AND REPAINT THE SIDEBAR. Setting the flag is not enough: the
     * asterisk is drawn by the shell, which has no way to know
     * unless it is told. */
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

int render_dirty(void) { return g_dirty; }

void render_show_first_tab(void)
{
    if (g_notebook != NULL)
        gtk_notebook_set_page(GTK_NOTEBOOK(g_notebook), 0);
}

/* Read the widgets into a struct. Shared by collect and by the
 * render itself, so the two cannot disagree about what is set. */
/*
 * THE MAIN PAGE'S `Soundfont:' LINE, RE-ASKED. It used to be refreshed
 * only inside gather() - i.e. when Render was PRESSED - and at build,
 * which is why it followed a Midi Settings change "some of the time
 * but not always" (design/36 row 34). Now also from render_machine().
 *
 * IT SHOWS WHAT WILL BE USED, WHICH IS THE ADVANCED TAB'S CHOICE WHEN
 * ONE HAS BEEN MADE - row 58. The label read vlhe_fonts()[0], the Midi
 * Settings font, while the render read font_chosen(g_gm), the Advanced
 * option menu; two sources, and the user proved which was right by
 * rendering MT32 and SC-55 to two different file sizes. So: the
 * Advanced choice if the widget exists and has one, else the Midi
 * Settings font, and say which.
 */
static void font_label_refresh(void)
{
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    const char *chosen;
    int nf;
    char buf[VLHE_PATH_MAX + 48];

    if (g_font == NULL)
        return;

    /*
     * `Use VLHE settings' TICKED MEANS THE MIDI SETTINGS FONT, and the
     * label said "(from Advanced)" regardless - the user, 2026-09-24:
     * "It was saying using Roland (from advanced) with the use vlhe
     * checked." The render path's rule (the g_use_vlhe test in the
     * render handler) is mirrored here: the Advanced choice counts
     * only when the switch is OFF.
     */
    chosen = (g_use_vlhe != NULL && GTK_TOGGLE_BUTTON(g_use_vlhe)->active)
               ? NULL : font_chosen(g_gm, 1);
    if (chosen != NULL && chosen[0] != '\0') {
        const char *base = strrchr(chosen, '/');
        const char *song = font_chosen(g_song, 1);

        sprintf(buf, "%.200s%s", base != NULL ? base + 1 : chosen,
                song != NULL ? STR_RND_TEXT_AND_SONG_FONT_FROM_ADVANCED
                             : STR_RND_TEXT_FROM_ADVANCED);
        gtk_label_set_text(GTK_LABEL(g_font), buf);
        return;
    }

    nf = vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL);
    if (nf > 0 && fonts[0].path[0] != '\0') {
        const char *base = strrchr(fonts[0].path, '/');

        sprintf(buf, "%.200s%s",
                base != NULL ? base + 1 : fonts[0].path,
                nf > 1 ? STR_RND_TEXT_AND_SONG_FONT_FROM_MIDI
                       : STR_RND_TEXT_FROM_MIDI_SETTINGS);
        gtk_label_set_text(GTK_LABEL(g_font), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(g_font),
                           STR_RND_LABEL_NONE_SET_CHOOSE_ONE);
    }
}

/*
 * THE ADVANCED TAB'S FONT LISTS, REFILLED. g_navail was filled once,
 * in build_advanced(), so a font found by a rescan or Add Folder did
 * not appear until the GUI was restarted (row 58: "It only has Roland
 * in it. It takes a restart"). The current choice is kept by PATH
 * across the refill, so a refresh does not silently change what will
 * be rendered.
 */
static void font_menus_refresh(void)
{
    char keep_gm[VLHE_PATH_MAX], keep_song[VLHE_PATH_MAX];
    const char *c;

    if (g_gm == NULL || g_song == NULL)
        return;

    keep_gm[0] = keep_song[0] = '\0';
    c = font_chosen(g_gm, 1);
    if (c != NULL) { strncpy(keep_gm, c, sizeof keep_gm - 1); keep_gm[sizeof keep_gm - 1] = '\0'; }
    c = font_chosen(g_song, 1);
    if (c != NULL) { strncpy(keep_song, c, sizeof keep_song - 1); keep_song[sizeof keep_song - 1] = '\0'; }

    /*
     * REBUILD THE MENUS ONLY IF THE LIST CHANGED - design/47 R6. This
     * runs on every show of the page and after every plan, and it
     * destroyed and recreated both menus' items each time whether or
     * not a font had appeared. The scan still runs (it is how a new
     * font is noticed); the widget churn does not.
     */
    {
        char   before[VLHE_MAX_AVAIL][VLHE_PATH_MAX];
        int    nbefore = g_navail, k, same;

        memcpy(before, g_avail, sizeof before);
        g_navail = vlhe_available_fonts(g_avail, VLHE_MAX_AVAIL);
        same = (g_navail == nbefore);
        for (k = 0; same && k < g_navail; k++)
            if (strcmp(before[k], g_avail[k]) != 0)
                same = 0;
        if (same && GTK_OPTION_MENU(g_gm)->menu != NULL)
            return;
    }

    /* An empty keep_gm is the follow entry, kept as itself - it used
     * to be replaced by Midi Settings' font as if chosen. */
    font_menu_fill(g_gm,   GM_FOLLOW_LABEL, keep_gm[0]   ? keep_gm   : NULL, NULL);
    font_menu_fill(g_song, SONG_NONE_LABEL, keep_song[0] ? keep_song : NULL, NULL);
}

void render_machine(void)
{
    font_menus_refresh();
    font_label_refresh();
}

static void
gather(struct vlhe_render *r)
{
    const char *f;

    memset(r, 0, sizeof *r);

    r->use_vlhe = g_use_vlhe != NULL &&
                  GTK_TOGGLE_BUTTON(g_use_vlhe)->active;
    r->voices = g_voices != NULL
        ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_voices))
        : 512;
    r->rate = rate_at(choice_index(g_rate));
    r->gain_milli = g_gain != NULL
        ? (int)(gtk_spin_button_get_value_as_float(
                    GTK_SPIN_BUTTON(g_gain)) * 1000.0 + 0.5)
        : 500;
    r->law     = choice_index(g_law);
    r->filter  = choice_index(g_vfilter);
    r->reverb  = g_reverb != NULL &&
                 GTK_TOGGLE_BUTTON(g_reverb)->active;
    r->chorus  = g_chorus != NULL &&
                 GTK_TOGGLE_BUTTON(g_chorus)->active;
    r->bypass  = g_bypass != NULL &&
                 GTK_TOGGLE_BUTTON(g_bypass)->active;
    r->modenv  = (g_modenv != NULL && choice_index(g_modenv) == 1)
                 ? VLHE_MODENV_REFERENCE : VLHE_MODENV_FAST;

    f = font_chosen(g_gm, 1);
    if (f != NULL) {
        strncpy(r->gm_font, f, sizeof r->gm_font - 1);
        r->gm_font[sizeof r->gm_font - 1] = '\0';
    }
    f = font_chosen(g_song, 1);
    if (f != NULL) {
        strncpy(r->song_font, f, sizeof r->song_font - 1);
        r->song_font[sizeof r->song_font - 1] = '\0';
    }

    /* WHERE LAME IS, kept so it need not be retyped - the user,
     * 2026-09-23. Corel ships none, so this is whatever they built
     * or fetched and nothing else can guess it. */
    if (g_lame != NULL) {
        const char *l = gtk_entry_get_text(GTK_ENTRY(g_lame));

        if (l != NULL) {
            strncpy(r->lame, l, sizeof r->lame - 1);
            r->lame[sizeof r->lame - 1] = '\0';
        }
    }
    if (g_wavtemp != NULL)
        r->wav_temp = choice_index(g_wavtemp);
}

int render_collect(void)
{
    struct vlhe_render r;

    gather(&r);
    if (vlhe_set_render(&r) != 0) {
        say(STR_RND_MSG_COULD_NOT_SAVE_RENDER);
        return -1;
    }
    g_dirty = 0;
    tabs_clear();
    return 0;
}

void render_reload(void)
{
    struct vlhe_render r;

    /*
     * THE SOUNDFONT LABEL FIRST, AND IT NEVER RELOADED - found on
     * target 2026-09-23. The user chose a font on the Midi page,
     * pressed Apply, and this page still said "none set - choose one
     * in Midi Settings and press Apply". They HAD.
     *
     * IT WAS SET ONCE WHEN THE PAGE WAS BUILT and never again, so the
     * message was stale rather than wrong - and the message this
     * evening made MORE confusing, because "press Apply" is exactly
     * what they had just done. The advice was right and the state
     * behind it was old.
     *
     * BEFORE THE `vlhe_render()' GUARD, on purpose: that call can
     * fail on a machine with no config and this label must still
     * catch up, since a font lives in the MIDI page's settings rather
     * than the render ones.
     */
    font_label_refresh();

    if (vlhe_render(&r) != 0)
        return;

    g_loading = 1;

    /*
     * THE SAVED LAME PATH, and a default when there is none.
     *
     * NOT SET YET means look beside ourselves, which in a portable
     * tree is where `lame' actually is - the same Tools= lookup
     * smf2wav uses. A user who unpacks the tarball then finds the
     * field already filled rather than being told to go and find a
     * binary that is sitting next to the program.
     *
     * A SAVED VALUE ALWAYS WINS, including one the user cleared on
     * purpose - guessing over an explicit choice is worse than an
     * empty box.
     */
    if (g_lame != NULL) {
        if (r.lame[0] != '\0') {
            gtk_entry_set_text(GTK_ENTRY(g_lame), r.lame);
        } else {
            char dir[VLHE_PATH_MAX];
            char cand[VLHE_PATH_MAX];
            struct stat st;

            if (vlhe_self_is_trial()
                && vlhe_self_subdir("Tools", dir, sizeof dir)
                && strlen(dir) + sizeof "/lame" < sizeof cand) {
                sprintf(cand, "%s/lame", dir);
                if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)
                    && access(cand, X_OK) == 0)
                    gtk_entry_set_text(GTK_ENTRY(g_lame), cand);
            }
        }
    }

    if (g_use_vlhe != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_use_vlhe),
                                     r.use_vlhe ? TRUE : FALSE);
    if (g_voices != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_voices),
                                  (float) r.voices);
    if (g_gain != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_gain),
                                  r.gain_milli / 1000.0);
    if (g_rate != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_rate),
                                    rate_index(r.rate));
    if (g_law != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_law), r.law);
    if (g_vfilter != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_vfilter),
                                    r.filter);
    if (g_wavtemp != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_wavtemp),
                                    r.wav_temp);
    if (g_reverb != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_reverb),
                                     r.reverb ? TRUE : FALSE);
    if (g_chorus != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_chorus),
                                     r.chorus ? TRUE : FALSE);
    if (g_bypass != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_bypass),
                                     r.bypass ? TRUE : FALSE);
    if (g_modenv != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_modenv),
                                    r.modenv == VLHE_MODENV_REFERENCE
                                    ? 1 : 0);

    /*
     * AND THE TWO FONT MENUS - design/47 R4, 2026-09-30. gather()
     * wrote GmFont and SongFont and nothing ever read them back, so
     * the menus showed the Midi page's fonts (build_advanced()'s
     * seed) whatever this page had saved, and the next Render used
     * what was showing. An EMPTY saved GmFont means "the Midi
     * Settings' own" (vlhe_backend.h), so that case follows the MIDI
     * page exactly as build and font_menus_refresh() do. The fill
     * re-reads the available list, so a font added since is seen too.
     */
    if (g_gm != NULL && g_song != NULL) {
        const char *gm = r.gm_font[0] ? r.gm_font : NULL;
        const char *song = r.song_font[0] ? r.song_font : NULL;

        /* An empty GmFont selects the follow entry - it no longer
         * substitutes Midi Settings' font into the menu as if chosen
         * (2026-10-02, see font_menu_fill()). */
        g_navail = vlhe_available_fonts(g_avail, VLHE_MAX_AVAIL);
        font_menu_fill(g_gm, GM_FOLLOW_LABEL, gm, NULL);
        font_menu_fill(g_song, SONG_NONE_LABEL, song, NULL);
    }

    g_loading = 0;
    g_dirty = 0;
    tabs_clear();
}

GtkWidget *
render_build(void (*report_fn)(const char *))
{
    GtkWidget *nb, *tab;

    g_report = report_fn;

    nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);

    tab = gtk_label_new(STR_RND_LABEL_RENDER);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_basic(), tab);

    tab = gtk_label_new(STR_RND_LABEL_ADVANCED);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_advanced(), tab);

    g_notebook = nb;

    /* THE SAVED VALUES, AFTER BOTH TABS EXIST. build_advanced() sets
     * the widgets from the Midi Settings so that an unconfigured
     * machine starts somewhere sensible; this then overrides them
     * with whatever [Render Settings] holds, which is what the user
     * last chose. */
    render_reload();

    gtk_widget_show(nb);
    return nb;
}
