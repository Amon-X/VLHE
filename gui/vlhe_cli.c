/*
 * vlhe_cli.c - the no-X half of vlhe.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * `vlhe volume' today; `vlhe apply', `vlhe status' and the rest as
 * they are built. design/33 section 1d has the design and why these
 * are SUBCOMMANDS rather than flags on the GUI:
 *
 *   - a flag modifies how a program does its job; a subcommand
 *     chooses the job, and `vlhe --apply' launches nothing
 *   - the GUI already takes `-m volume' to open a page, so `-volume'
 *     beside `--volume' would differ by one character for entirely
 *     different work - the user spotted that
 *   - each subcommand wants its own arguments, where flags taking two
 *     each start caring about order
 *
 * THE PARSER RULE IS ONE LINE: a bare argv[1] is a subcommand,
 * because every GUI flag has a dash and no subcommand does.
 *
 * WHY A SEPARATE FILE FROM THE GUI. This links no toolkit. A headless
 * machine, a serial console or an X server that will not start are
 * all real on this target, and needing GTK to read a volume level is
 * the thing `vsoundvol' got right and must not be lost when it
 * retires (design/07).
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* geteuid, execv */
#include <sys/types.h>
#include <sys/stat.h>

#include "vlhe_conf.h"
#include "vlhe_backend.h"
#include "vlhe_apply.h"
#include "vlhe_journal.h"    /* vlhe_journal_path, for problem messages */
#include "vlhe_cli.h"
#include "vlhe_self.h"

/* ------------------------------------------------------------------ */
/* volume                                                             */
/* ------------------------------------------------------------------ */

/*
 * A BAR, FOR READING. vsoundvol draws one and it is the nicer thing
 * to look at; -q gives columns for scripts, which vsoundvol has no
 * equivalent of at all.
 */
static void
draw_bar(int vol)
{
    /* FULL AT 100, which is unity; a boosted level fills the bar and
     * the number beside it says how far past. */
    int filled = (vol > 100 ? 100 : vol) * 20 / 100;
    int i;

    putchar('[');
    for (i = 0; i < 20; i++)
        putchar(i < filled ? '#' : '-');
    putchar(']');
}

/*
 * ONE CHANNEL.
 *
 * A MUTED CHANNEL SHOWS ITS LEVEL, NEVER 0 - design/33 section 1d,
 * and it is the whole point of mute being a flag beside `vol' rather
 * than a volume of zero (vsound_chan.h, VSOUND_CHN_MUTED). Printing 0
 * would throw away the distinction the driver keeps.
 */
static void
show_channel(const struct vlhe_channel *c, int plain)
{
    const char *name;

    if (c->pid == 0) {
        name = c->midi ? "(reserved for MIDI)" : "(free)";
        if (plain)
            printf("%d\t-\t-\t-\n", c->index);
        else
            printf("  %d  %-16s %s\n", c->index, "-", name);
        return;
    }

    name = c->name[0] ? c->name : "?";

    if (plain) {
        /*
         * TAB SEPARATED, FIXED COLUMNS: index, name, volume, state.
         * A script can cut this; it cannot cut a bar graph, which is
         * why vsoundvol's output has no machine-readable form.
         */
        printf("%d\t%s\t%d\t%s\n", c->index, name, c->volume,
               c->muted ? "muted" : "on");
        return;
    }

    printf("  %d  %-16s ", c->index, name);
    draw_bar(c->volume);
    printf(" %3d%%", c->volume);
    if (c->muted)
        printf("  muted");
    if (c->midi)
        printf("  MIDI");
    putchar('\n');
}

/*
 * LIVENESS IS NOT REPORTED HERE, deliberately. `active' now means
 * "produced sound since the last poll" (vsound.h, `mixed'), which a
 * one-shot command cannot evaluate without sampling twice and
 * pausing - and a volume listing that pauses is surprising.
 * `vlhe status' is its home.
 *
 * vsoundvol prints "playing" from VSOUND_CI_RUNNING, which was found
 * unreliable on 2026-09-18: it means "has blocked on a write", so a
 * small-buffer synth reads as not playing while audible.
 */
/*
 * SETTING A LEVEL. `vlhe volume N PERCENT', `vlhe volume N mute'.
 *
 * WHY THE PID IS READ FIRST AND PASSED BACK. vsound reuses channel
 * indices: a client exits, another opens, and the slot number is the
 * same. The driver checks the pid under one cli() (vsound.h,
 * VSOUND_IOC_VOL), so a write carries the pid the caller last saw and
 * -ESRCH means "that slot changed hands" - which for a one-shot
 * command is a race the user should be told about plainly, not a
 * failure to retry behind their back.
 *
 * The window here is ONE vlhe_channels() call wide, far tighter than
 * the GUI's 250 ms poll, but it is not zero and the guard is the same
 * one either way.
 */
static int
set_one(int index, const char *what)
{
    struct vlhe_channel ch[VLHE_MAX_CHAN];
    int n, i, found = -1;
    int mute = -1;              /* 1 mute, 0 unmute, -1 a level */
    long level = -1;

    /*
     * PARSE BEFORE PROBING. A bad argument is the user's typo and
     * should be named as one, whatever the machine's state - saying
     * "the vsound module is not loaded" in reply to `volume 1 loud'
     * answers a question nobody asked and hides the real mistake.
     */
    if (strcmp(what, "mute") == 0)
        mute = 1;
    else if (strcmp(what, "unmute") == 0)
        mute = 0;
    else {
        char *end;

        /* STRTOL, NOT ATOI - `vlhe volume 0 loud' must be an error
         * rather than silently setting zero. */
        level = strtol(what, &end, 10);
        if (end == what || *end != '\0' || level < 0
            || level > VLHE_VOL_BOOST) {
            fprintf(stderr, "vlhe volume: '%s' is not 0-%d, `mute'"
                            " or `unmute'\n", what, VLHE_VOL_BOOST);
            return 2;
        }
    }

    if (!vlhe_sound_present()) {
        fprintf(stderr, "vlhe volume: the vsound module is not loaded\n");
        return 1;
    }

    n = vlhe_channels(ch, VLHE_MAX_CHAN);
    for (i = 0; i < n; i++) {
        if (ch[i].index == index) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        fprintf(stderr, "vlhe volume: no channel %d (there are %d)\n",
                index, n);
        return 1;
    }

    /* A FREE SLOT HAS NOTHING TO SET, and saying so beats -ESRCH from
     * the driver, which reads like a failure rather than an empty
     * chair. */
    if (ch[found].pid == 0) {
        fprintf(stderr, "vlhe volume: channel %d is not in use\n", index);
        return 1;
    }

    if (mute >= 0) {
        if (vlhe_set_mute(index, ch[found].pid, mute) != 0) {
            fprintf(stderr, "vlhe volume: could not %s channel %d"
                            " (did it just exit?)\n", what, index);
            return 1;
        }
        printf("channel %d (%s) %s\n", index,
               ch[found].name[0] ? ch[found].name : "?",
               mute ? "muted" : "unmuted");
        return 0;
    }

    if (vlhe_set_volume(index, ch[found].pid, (int)level) != 0) {
        fprintf(stderr, "vlhe volume: could not set channel %d"
                        " (did it just exit?)\n", index);
        return 1;
    }

    /* THE LEVEL IS REPORTED BACK EVEN WHEN MUTED, and the mute is NOT
     * cleared - mute is a flag beside vol, not a volume of zero
     * (vsound_chan.h). Setting a level on a muted channel arms it for
     * the unmute, which is what a hardware mixer does. */
    printf("channel %d (%s) set to %d%%%s\n", index,
           ch[found].name[0] ? ch[found].name : "?", (int)level,
           ch[found].muted ? " (still muted)" : "");
    return 0;
}

static int
cmd_volume(int argc, char **argv)
{
    struct vlhe_channel ch[VLHE_MAX_CHAN];
    int n, i;
    int plain = 0;

    /* A BARE NUMBER IS A CHANNEL, so `vlhe volume 1 50' sets and
     * `vlhe volume' reports. No flag for the set: the shape of the
     * arguments already says which is meant, and a -s would be one
     * more thing to remember for the commoner of the two jobs. */
    if (argc >= 1 && argv[0][0] != '-') {
        char *end;
        long  idx = strtol(argv[0], &end, 10);

        if (end == argv[0] || *end != '\0' || idx < 0 || idx >= VLHE_MAX_CHAN) {
            fprintf(stderr, "vlhe volume: '%s' is not a channel number\n",
                    argv[0]);
            return 2;
        }
        if (argc < 2) {
            fprintf(stderr, "vlhe volume: channel %ld - set it to what?\n",
                    idx);
            fprintf(stderr, "  vlhe volume %ld 50        half\n", idx);
            fprintf(stderr, "  vlhe volume %ld mute\n", idx);
            return 2;
        }
        if (argc > 2) {
            fprintf(stderr, "vlhe volume: too many arguments\n");
            return 2;
        }
        return set_one((int)idx, argv[1]);
    }

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--plain") == 0)
            plain = 1;
        else {
            fprintf(stderr, "vlhe volume: unknown argument '%s'\n", argv[i]);
            fprintf(stderr,
                "usage: vlhe volume [-q]            report every channel\n"
                "       vlhe volume N LEVEL         set channel N's level,\n"
                "                                   0-100, above 100 boosts\n"
                "       vlhe volume N mute|unmute\n"
                "\n"
                "  -q  plain columns for scripts, rather than bars\n"
                "\n"
                "  Each program's level is remembered while VLHE is\n"
                "  loaded; Save Program Levels keeps them across a\n"
                "  reload. Setting one by program name from here is\n"
                "  not built yet.\n");
            return 2;
        }
    }

    if (!vlhe_sound_present()) {
        fprintf(stderr, "vlhe volume: the vsound module is not loaded\n");
        return 1;
    }

    n = vlhe_channels(ch, VLHE_MAX_CHAN);

    if (!plain) {
        printf("\n");
        printf("vsound channels\n\n");
    }

    for (i = 0; i < n; i++)
        show_channel(&ch[i], plain);

    if (!plain) {
        printf("\n");
        /* THE SAME CAVEAT vsoundvol prints, and it is worth keeping:
         * these are per-stream levels within our own mix, not the
         * card's output level. */
        printf("  These set streams against each other. For overall\n");
        printf("  loudness use the sound card's own mixer.\n");
        printf("\n");
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* setup                                                             */
/* ------------------------------------------------------------------ */

/*
 * WHERE THE SCRIPT LIVES: /usr/sbin, NOT /usr/bin.
 *
 * It is root-only and system-configuring, which is what sbin means -
 * and the practical effect is that it sits on root's PATH and not on
 * an ordinary user's, which is honest about who it is for. `vlhe'
 * (this CLI) installs to $(sbindir) beside it; only the GUI's link,
 * vlhe.gtk, goes in $(bindir), because anyone may run the GUI.
 *
 * Overridable for testing, like every other path here.
 *
 * BESIDE THIS BINARY FIRST - 2026-10-02, the user's "fix that vlhe
 * setup too". This was a fixed /usr/sbin path, right for the .deb and
 * wrong for a source install (make install's default prefix puts both
 * in /usr/local/sbin) and for a portable folder (setup-vlhe sits beside
 * vlhe). make install puts the two in the same $(sbindir), so
 * "beside me" answers every case; /usr/sbin and /usr/local/sbin follow.
 *
 * BUT ONLY A FILE THAT ROOT OR THE CALLER OWNS AND NOBODY ELSE CAN
 * WRITE. `vlhe setup' runs it as ROOT, and a portable folder may be
 * mode 1777 so that every user can save settings in it (mkxfer.sh does
 * that on the 86Box image): a setup-vlhe someone else put there must
 * not be what root runs. The fixed paths are taken as they are - they
 * are root's directories.
 */
static int
setup_ok(const char *path, int check_owner)
{
    struct stat sb;

    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)
        || access(path, X_OK) != 0)
        return 0;
    if (check_owner
        && ((sb.st_uid != 0 && sb.st_uid != getuid())
            || (sb.st_mode & (S_IWGRP | S_IWOTH)) != 0))
        return 0;
    return 1;
}

static const char *
setup_path(void)
{
    static char beside[VLHE_PATH_MAX];
    const char *v = getenv("VLHE_SETUP");

    if (v != NULL && *v != '\0')
        return v;
    if (vlhe_self_path("setup-vlhe", beside, sizeof beside)
        && setup_ok(beside, 1))
        return beside;
    if (setup_ok("/usr/sbin/setup-vlhe", 0))
        return "/usr/sbin/setup-vlhe";
    if (setup_ok("/usr/local/sbin/setup-vlhe", 0))
        return "/usr/local/sbin/setup-vlhe";
    /* NONE FOUND: name the packaged path, so the message that follows
     * says where it was expected. */
    return "/usr/sbin/setup-vlhe";
}

/*
 * `vlhe setup' - HAND OVER TO THE SCRIPT, if this is root.
 *
 * WHY EXEC AND NOT FORK: there is nothing to do afterwards. The
 * script owns the terminal from here, its exit status should be
 * ours, and a wrapper waiting around would only get in the way of a
 * program that reads from stdin.
 *
 * WHY CHECK ROOT FIRST rather than letting the script fail: it would
 * fail LATE, after the user had answered every question, because the
 * permission problem only appears when it tries to write
 * /etc/vlhe.conf. Refusing up front costs them nothing; refusing at
 * the end costs them the whole session.
 */
/*
 * WHAT IS LOADED AND WHAT IS RUNNING.
 *
 * THE SAME CALL THE GUI'S STATUS PAGE MAKES - vlhe_components(),
 * which reads /proc/modules and the pidfiles. Not a second opinion
 * and not the init script's: the machine is the authority, so a
 * daemon that died leaves a stale pidfile and this says so, where a
 * script's record would still claim it had started three.
 *
 * IT EXISTS BECAUSE THE GUI CANNOT ALWAYS BE REACHED. `status' is
 * what someone runs after a failed boot, which is exactly when X may
 * be down - and `/etc/init.d/vlhe status' execs this after printing
 * its own stamp, so the chain was built and only this link was a
 * stub. It printed "not built yet" until 2026-09-22.
 *
 * NEEDS NO ROOT. Reading /proc and stat()ing a pidfile is allowed to
 * anyone, which matters for a portable copy run by an ordinary user.
 */
static int
cmd_status(int argc, char **argv)
{
    /* VLHE_MAX_COMPONENTS, NOT 16. The header says 8 and this said
     * 16 in two places - oversized, so harmless today, but it is two
     * independent numbers where one constant already exists. The GUI
     * uses the constant; raising it would have left the CLI showing a
     * different list from the Status page, with nothing to say so. */
    struct vlhe_component c[VLHE_MAX_COMPONENTS];
    int n, i, down = 0;

    (void)argc;
    (void)argv;

    /* WHAT THE LAST BOOT LEARNED, ONCE - design/54 7h decision 2. The
     * boot's output went to /dev/null; this is where it is told. */
    {
        FILE *t = tmpfile();

        if (t != NULL) {
            if (vlhe_apply_notice_take(t)) {
                char line[512];

                rewind(t);
                printf("NOTICE (shown once):\n");
                while (fgets(line, sizeof line, t) != NULL)
                    printf("  %s", line);
                printf("  The full account is in %s.\n\n",
                       vlhe_journal_path());
            }
            fclose(t);
        }
    }

    /* AN UNFINISHED UNLOAD, and how to finish it from here - the boot's
     * notice says what happened and leaves the how to the front end
     * (the user, 2026-10-04: the GUI and the CLI each name their own). */
    {
        int left = vlhe_apply_leftover(NULL, NULL);
        int nfailed = vlhe_apply_leftover_failed();

        /* FAILED IS NOT "NEVER UNLOADED" - the user, 2026-10-04: an
         * unload tried those and could not; only OPEN ones were never
         * undone. */
        if (left > 0)
            printf("UNFINISHED UNLOAD: %d change(s) %s are still on"
                   " record - `vlhe apply -u' finishes it; until then"
                   " `vlhe apply' refuses. The full account is in %s.\n\n",
                   left,
                   nfailed == 0 ? "from a load that was never unloaded"
                   : nfailed == left ? "the last unload could not undo"
                   : "from a load that was not fully unloaded (some the"
                     " unload could not undo, the rest never undone)",
                   vlhe_journal_path());
    }

    /* WHAT NEEDS ATTENTION - design/54 7h Stage 3.5: shown every time
     * until it is put right, unlike the notice. */
    {
        struct vlhe_finding f[8];
        int nf = vlhe_apply_findings(f, 8), k;

        if (nf > 0) {
            printf("NEEDS ATTENTION:\n");
            for (k = 0; k < nf; k++)
                printf("  %s\n", f[k].text);
            printf("  `vlhe repair' asks about each one.\n\n");
        }
    }

    n = vlhe_components(c, VLHE_MAX_COMPONENTS);
    if (n <= 0) {
        fprintf(stderr, "vlhe status: nothing to report\n");
        return 1;
    }

    /* WHERE THE SETTINGS ARE COMING FROM, first. On a portable copy
     * that is the folder rather than /etc, and someone debugging why
     * a change did nothing needs to know which file was read before
     * anything else on this page means much. */
    printf("config:  %s\n", vlhe_conf_system_path());
    {
        const char *u = vlhe_conf_user_path();

        if (u != NULL)
            printf("         %s\n", u);
    }
    printf("\n");

    for (i = 0; i < n; i++) {
        const char *state;

        if (c[i].kind == VLHE_COMP_MODULE)
            state = c[i].present ? "loaded" : "not loaded";
        else if (c[i].present)
            state = "running";
        else
            state = "not running";

        if (!c[i].present)
            down++;

        /* THE PID IN THE STATE COLUMN, as the GUI shows it - a
         * running daemon without one would be odd, and the detail
         * column is where "stale pid file - it died" goes. */
        if (c[i].present && c[i].pid > 0)
            printf("  %-10s running (%d)  %s\n",
                   c[i].name, c[i].pid, c[i].detail);
        else
            printf("  %-10s %-13s %s\n",
                   c[i].name, state, c[i].detail);
    }

    /*
     * THE EXIT STATUS IS A RESULT, not decoration. `vlhe status' in a
     * script should be able to ask "is it all up?" without parsing
     * this, which is the ordinary convention for a status verb.
     */
    return down > 0 ? 1 : 0;
}

static int
cmd_setup(int argc, char **argv)
{
    const char *path = setup_path();
    char       *args[2];

    (void) argv;

    if (argc > 0) {
        fprintf(stderr, "vlhe setup: takes no arguments\n");
        fprintf(stderr, "  (to write somewhere else, run %s -f FILE)\n",
                path);
        return 2;
    }

    if (geteuid() != 0) {
        fprintf(stderr,
            "vlhe setup: this needs root - it writes /etc/vlhe.conf.\n"
            "\n"
            "  Run it as root:   su -c %s\n"
            "\n"
            "  Your own settings do not need root, and the control\n"
            "  centre writes those to ~/.vlhe/vlhe.conf as you.\n",
            path);
        return 1;
    }

    /* CONST CAST, AND IT IS SAFE: execv does not modify argv, the
     * prototype simply predates const. */
    args[0] = (char *) path;
    args[1] = (char *) 0;

    execv(path, args);

    /* Only reached if exec failed. */
    fprintf(stderr, "vlhe setup: cannot run %s\n", path);
    fprintf(stderr, "  Is the package fully installed?\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* apply                                                              */
/* ------------------------------------------------------------------ */

/*
 * `vlhe apply' - THE PIECE THAT MAKES A CONFIG MEAN ANYTHING.
 *
 * design/33 section 1d: nothing currently reads a config and acts on
 * it, so every persisted setting means "next time someone starts the
 * daemons by hand with matching flags". This closes that.
 *
 * ONLY `-n' IS BUILT. The plan - which module first, which parameter
 * where - is the part with all the decisions in it, and it is a pure
 * function of the config that a host test can check line by line.
 * The executing half is a thin loop over the same list and comes
 * next; splitting them means the untestable part is as small as
 * possible.
 *
 * THE MACHINE CHECK RUNS EVEN FOR `-n'. Not because a dry run could
 * damage anything - it prints and exits - but because the check
 * itself is worth testing, and someone who runs `vlhe apply -n' on
 * the wrong box should learn that THERE rather than after dropping
 * the flag.
 */
/*
 * DRIFT FROM THE BASELINE, ASKED ON A TERMINAL - design/54 7h Stage 3.3,
 * in the dialog's words since 2026-10-04. The list is already printed
 * above the question.
 */
static int
cli_drift_ask(const struct vlhe_drift *d, int n)
{
    char line[32];

    (void) d;
    printf("\n%d path(s) above have changed since VLHE first saw them.\n"
           "[a]ccept changes (the new baseline) / [u]ndo changes (put"
           " back) / [l]oad anyway / [C]ancel? ", n);
    fflush(stdout);
    /* CANCEL IS THE DEFAULT - an empty line or anything else - as the
     * dialog's is: the other three each change something (2026-10-04). */
    if (fgets(line, sizeof line, stdin) == NULL)
        return VLHE_DRIFT_CANCEL;
    if (line[0] == 'a' || line[0] == 'A')
        return VLHE_DRIFT_UPDATE;
    if (line[0] == 'u' || line[0] == 'U')
        return VLHE_DRIFT_UNDO;
    if (line[0] == 'l' || line[0] == 'L')
        return VLHE_DRIFT_KEEP;
    return VLHE_DRIFT_CANCEL;
}

static int
cmd_apply(int argc, char **argv)
{
    struct vlhe_plan plan;
    char  why[256];
    int   dry = 0;
    int   unload = 0;
    int   boot = 0;     /* --boot: the init script's start */
    int   i, rc;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0)
            dry = 1;
        else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--unload") == 0)
            unload = 1;
        else if (strcmp(argv[i], "--boot") == 0)
            boot = 1;
        else {
            fprintf(stderr, "vlhe apply: unknown argument '%s'\n", argv[i]);
            fprintf(stderr,
                "usage: vlhe apply           load and start everything\n"
                "       vlhe apply -u        stop and unload it\n"
                "       vlhe apply -n        show what would be done\n"
                "       vlhe apply -n -u     ...for the unload side\n"
                "       vlhe apply --boot    the init script's load: first\n"
                "                            finish a load never unloaded\n");
            return 2;
        }
    }

    if (vlhe_apply_machine_ok(why, sizeof why) != 0) {
        fprintf(stderr, "vlhe apply: refusing - %s\n", why);
        fprintf(stderr,
            "\n"
            "  This loads kernel modules built for a Linux 2.2 kernel.\n"
            "  There is deliberately no way to override this.\n");
        return 1;
    }

    /* A NEGATIVE BUILD IS A BUG, NOT A FILE - design/47 A7. The only
     * way vlhe_plan_build() returns < 0 is a NULL plan or an unknown
     * scope, neither of which a config can cause, so "could not read
     * the configuration" could never have been true. An unreadable
     * OR MISSING config builds a plan from the defaults -
     * vlhe_apply_machine_ok() checks the kernel version only. (This
     * said a missing one was refused there; corrected 2026-10-02,
     * when `vlhe-init stop' began relying on `apply -u' working with
     * no config, which it does: the unload plan acts on what is
     * loaded, not on settings.) */
    if (vlhe_plan_build(&plan, unload) < 0) {
        fprintf(stderr, "vlhe apply: internal error - no plan could be"
                        " built (this is a bug, please report it)\n");
        return 1;
    }

    /* THE CONFIG ACTUALLY IN USE - design/47 A6. This printed
     * "/etc/vlhe.conf" as a literal, portable mode included, where the
     * file is the one beside the program. */
    if (dry) {
        printf("%s %s would do this:\n\n",
               unload ? "Unloading" : "Applying", vlhe_system_conf_path());
        vlhe_plan_print(&plan, stdout);
        printf("\n");
        return 0;
    }

    /*
     * ROOT FIRST, OR NOTHING - design/55 R14, design/54 D56 (2026-10-04).
     * Without it a user's `vlhe apply' ran into the first step's insmod,
     * stopped, and then said "Earlier steps have already run ... The full
     * account is in /var/log/vlhe/changes", with nothing run and nothing
     * journalled. Asked by raising, as vlhe_reset_settings() does, so the
     * setuid build - which holds root only inside a raise - still passes.
     */
    {
        int ok;

        vlhe_root_begin();
        ok = vlhe_is_root();
        vlhe_root_end();
        if (!ok) {
            fprintf(stderr, "vlhe apply: loading and unloading need root -"
                            " nothing was run. `vlhe apply -n' shows what"
                            " it would do.\n");
            return 1;
        }
    }

    /*
     * FOR REAL. THE PLAN IS PRINTED FIRST AND THEN RUN, so the
     * terminal holds a record of what was attempted even if a step
     * wedges the machine - which is the case this output exists for.
     * A dry run someone reads and a real run someone later reads the
     * scrollback of should look the same.
     */
    printf("%s %s:\n\n", unload ? "Unloading" : "Applying",
           vlhe_system_conf_path());
    vlhe_plan_print(&plan, stdout);
    printf("\n");
    fflush(stdout);

    /*
     * AT BOOT, FINISH WHAT A LOAD THAT WAS NEVER UNLOADED LEFT - design/54
     * 7h decision 2. The init script passes --boot; this output goes to
     * /dev/null there, so vlhe_apply_boot_finish() also writes the
     * one-time notice the Status page and `vlhe status' show. Not
     * finished - [Boot] FinishLeftover 0, or something FAILED - and the
     * load does not start a session over it.
     */
    if (boot && !unload && vlhe_apply_boot_finish(stdout) < 0) {
        printf("\nnot loaded: a previous load was not finished - see"
               " `vlhe status'. The full account is in %s.\n",
               vlhe_journal_path());
        return 1;
    }

    /* THE KERNEL-LOG CAPTURE - [Tracing] Capture: opened before a load,
     * closed after an unload (after the teardown, so rmmod's lines are
     * inside the run). The init script comes through here too. */
    if (!unload)
        (void) vlhe_capture_begin(stdout);

    /* DRIFT IS ASKED ONLY OF A PERSON - on a terminal and not at boot;
     * otherwise it is warned about and the baseline kept (7h Stage 3.3). */
    if (!boot && isatty(0) && isatty(1))
        vlhe_apply_set_drift_ask(cli_drift_ask);
    rc = vlhe_plan_run(&plan, stdout);

    if (unload)
        (void) vlhe_capture_end(stdout);

    /* AND WHAT DID NOT COME BACK - a "Load at startup" drive whose
     * image could not be opened at boot (its disk not mounted yet).
     * vdiscd said so to /dev/null; the one-time notice is where it is
     * seen (2026-10-05). */
    if (boot && !unload && rc == 0)
        (void) vlhe_apply_boot_drives(stdout);

    printf("\n");
    if (rc == 0) {
        printf("done.\n");
        return 0;
    }
    if (rc < 0) {
        printf("nothing was run. The full account is in %s.\n",
               vlhe_journal_path());
        return 1;
    }

    /*
     * NAME THE STEP THAT STOPPED IT. "apply failed" sends someone to
     * read the whole plan again; "step 3 failed" points at the line.
     */
    printf("STOPPED at step %d of %d:\n    %s\n",
           rc, plan.n, plan.step[rc - 1].cmd);
    if (rc == 1)
        printf("\nNothing before it had run. The full account is in"
               " %s.\n", vlhe_journal_path());
    else
        printf("\nEarlier steps have already run. `vlhe apply -u' tears "
               "down\nwhat is up. The full account is in %s.\n",
               vlhe_journal_path());
    return 1;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

void
vlhe_cli_usage(FILE *fp)
{
    /* THE CONTROL CENTRE IS vlhe.gtk, NOT A BARE vlhe - design/54 D16,
     * 2026-10-03. This line said "vlhe   the control centre (needs X)",
     * which was true only of the GUI binary run with no arguments; the
     * installed `vlhe' is this program, and with no arguments it prints
     * this and exits 2. */
    fprintf(fp,
        "usage: vlhe <subcommand>\n"
        "\n"
        "  The control centre is a separate program: vlhe.gtk (needs X).\n"
        "\n"
        "  vlhe volume [-q]  report every channel's level and mute\n"
        "  vlhe volume N 50  set channel N to 50 per cent\n"
        "  vlhe volume N mute|unmute\n"
        "\n"
        "  vlhe apply        load the modules and start the daemons\n"
        "  vlhe apply -u     stop and unload them\n"
        "  vlhe apply -n     show what that would do, change nothing\n"
        "  vlhe reload       the running synth takes the current\n"
        "                    settings, without a restart\n"
        "  vlhe status       what is loaded and running, and what\n"
        "                    needs attention\n"
        "  vlhe repair       ask about each thing that needs attention\n"
        "\n"
        "Planned, not built:\n"
        "  vlhe attach N IMG put a disc image in drive N\n"
        "\n"
        "Root only:\n"
        "  vlhe setup        the install-time configurator - writes\n"
        "                    /etc/vlhe.conf. Runs setup-vlhe from beside\n"
        "                    this program, or /usr/sbin.\n");
}

/*
 * IS argv[1] A SUBCOMMAND? Returns 1 if this run belongs to the CLI
 * and the GUI must not start.
 */
/*
 * vlhe repair - design/54 7h Stage 3.5 (the user: "Yes for vlhe repair").
 * Shows each finding and asks about it, one at a time, so putting a
 * device node back from a terminal is a deliberate act per item - the
 * same choice the Status page offers. Refuses without a terminal: it
 * only ever asks.
 */
static int
cmd_repair(int argc, char **argv)
{
    struct vlhe_finding f[8];
    char line[VLHE_PATH_MAX];
    int  n, i, bad = 0;

    (void) argv;
    if (argc != 0) {
        fprintf(stderr, "usage: vlhe repair    ask about each thing that"
                        " needs attention\n");
        return 2;
    }
    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "vlhe repair: it asks about each change, so it"
                        " needs a terminal\n");
        return 2;
    }
    n = vlhe_apply_findings(f, 8);
    if (n == 0) {
        printf("Nothing needs attention.\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        printf("\n%s.\n", f[i].text);
        if (f[i].kind == VLHE_FIND_DSP) {
            printf("Put back the stock device node [s], keep it as it is"
                   " [k], or leave it for now [N]? ");
            fflush(stdout);
            if (fgets(line, sizeof line, stdin) == NULL)
                break;
            if (line[0] == 's' || line[0] == 'S') {
                if (vlhe_apply_fix_finding(f[i].path, f[i].kind,
                                           VLHE_FIX_STOCK, NULL, stdout) != 0)
                    bad = 1;
            } else if (line[0] == 'k' || line[0] == 'K') {
                if (vlhe_apply_fix_finding(f[i].path, f[i].kind,
                                           VLHE_FIX_KEEP, NULL, stdout) != 0)
                    bad = 1;
            } else {
                printf("left as it is\n");
            }
        } else {
            char drv[8][VLHE_PATH_MAX];
            int  nd = vlhe_apply_cd_drives(drv, 8), k, pick;

            printf("CD drives found:");
            for (k = 0; k < nd; k++)
                printf("  %d) %s", k + 1, drv[k]);
            if (nd == 0)
                printf(" none");
            printf("\nPoint /dev/cdrom at a drive [its number, or a device"
                   " path], remove the link [r], or leave it for now [N]? ");
            fflush(stdout);
            if (fgets(line, sizeof line, stdin) == NULL)
                break;
            line[strcspn(line, "\n")] = '\0';
            pick = atoi(line);
            if (pick >= 1 && pick <= nd) {
                if (vlhe_apply_fix_finding(f[i].path, f[i].kind,
                                           VLHE_FIX_DRIVE, drv[pick - 1],
                                           stdout) != 0)
                    bad = 1;
            } else if (line[0] == '/') {
                if (vlhe_apply_fix_finding(f[i].path, f[i].kind,
                                           VLHE_FIX_DRIVE, line, stdout) != 0)
                    bad = 1;
            } else if (line[0] == 'r' || line[0] == 'R') {
                if (vlhe_apply_fix_finding(f[i].path, f[i].kind,
                                           VLHE_FIX_REMOVE, NULL, stdout) != 0)
                    bad = 1;
            } else {
                printf("left as it is\n");
            }
        }
    }
    return bad ? 1 : 0;
}

/*
 * vlhe reload - RUNNING DAEMONS ADOPT THE CURRENT SETTINGS, NO RESTART.
 * 2026-10-06, the user: the control centre's "Apply settings" button had
 * no command-line counterpart, so a saved rate change reached a running
 * synth only by `vlhe apply -u' and `vlhe apply' - which drops whatever
 * is playing. A separate command rather than an `apply' flag: apply
 * changes the system and is journalled, this only tells a daemon. Named
 * as init scripts and daemons name re-reading settings.
 *
 * vlhe_apply_synth() IS THE BUTTON'S OWN CODE, so the two cannot drift.
 * Only the synth has settings a running process can adopt; the rate
 * waits for its next release, and a SoundFont is not among them.
 */
static int
cmd_reload(int argc, char **argv)
{
    int rc;

    (void) argv;
    if (argc > 0) {
        fprintf(stderr, "usage: vlhe reload\n");
        return 2;
    }
    rc = vlhe_apply_synth();
    if (rc < 0) {
        printf("vmidid is not running - nothing to reload."
               " `vlhe apply' starts it.\n");
        return 1;
    }
    if (rc > 0) {
        printf("vmidid refused %d setting(s) - the rest are in use."
               " /var/log/vlhe/DAEMON.LOG says which.\n", rc);
        return 1;
    }
    printf("vmidid: the settings are in use now. The sample rate changes\n"
           "at its next pause, once the last note has died away. A\n"
           "different SoundFont needs `vlhe apply -u', then `vlhe apply'.\n");
    return 0;
}

int
vlhe_cli_is_subcommand(const char *arg)
{
    if (arg == NULL || arg[0] == '-' || arg[0] == '\0')
        return 0;
    return 1;
}

int
vlhe_cli_main(int argc, char **argv)
{
    /* Root takes no overrides from its environment - design/55
     * recommendation 3, vlhe_self.h. First, so no subcommand reads one;
     * vlhe.gtk has already done it when it dispatches here, and it is
     * idempotent. */
    vlhe_self_root_env();

    if (argc < 2) {
        vlhe_cli_usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "volume") == 0)
        return cmd_volume(argc - 2, argv + 2);

    /*
     * NAMED RATHER THAN LUMPED WITH A TYPO. Someone typing a
     * subcommand this project has designed and not built should be
     * told that, not told it is unknown - the design documents
     * promise these and a user may have read them.
     */
    /* THE COMMAND LINE PLANS FROM LoadAtBoot - it is what the init
     * script runs, and what a person typing it expects boot to do.
     * The control centre plans from its Include boxes. 2026-10-02. */
    vlhe_plan_from_boot_keys(1);
    vlhe_apply_set_frontend(VLHE_FRONT_CLI);    /* its messages, 2026-10-04 */
    if (strcmp(argv[1], "apply") == 0)
        return cmd_apply(argc - 2, argv + 2);
    if (strcmp(argv[1], "reload") == 0)
        return cmd_reload(argc - 2, argv + 2);
    if (strcmp(argv[1], "status") == 0)
        return cmd_status(argc - 2, argv + 2);
    if (strcmp(argv[1], "repair") == 0)
        return cmd_repair(argc - 2, argv + 2);
    if (strcmp(argv[1], "setup") == 0)
        return cmd_setup(argc - 2, argv + 2);

    fprintf(stderr, "vlhe: unknown subcommand '%s'\n", argv[1]);
    vlhe_cli_usage(stderr);
    return 2;
}
