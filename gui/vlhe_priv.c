/*
 * vlhe_priv.c - the setuid build's privilege handling. Read
 * vlhe_priv.h first: it has why setuid, why the drop comes before
 * gtk_init(), and where the shape came from.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>

#include "vlhe_priv.h"

#ifdef VLHE_SETUID

static int g_dropped;           /* we gave up euid 0 at startup      */
static int g_unlocked;          /* the password was given            */
static int g_depth;             /* nested vlhe_priv_raise() calls    */

/*
 * THE ENVIRONMENT IS REBUILT FROM AN ALLOW-LIST - design/48 S3,
 * design/49 N3, N4, N5. 2026-09-30.
 *
 * THIS WAS A DENY-LIST OF NAMES SET TO EMPTY, and both halves were
 * wrong:
 *
 *   - A DENY-LIST IS NEVER COMPLETE. S3 found six VLHE_* variables it
 *     missed, N5 three more (VDISCD_CTL_DIR, VMIDID_CTL_DIR,
 *     VLHE_CDDB_DIRS), and the next variable anyone adds would be the
 *     next gap. An allow-list fails closed: a new variable is simply
 *     absent until someone decides it is safe.
 *   - EMPTY IS NOT ABSENT. The old comment said GTK treats an empty
 *     value as unset; it does not - `if (env_string)' is true for ""
 *     (gtkmain.c:215, gtkrc.c:318). So blanking GTK_RC_FILES made GTK
 *     skip its default rc files entirely (N3), and blanking
 *     VLHE_PROC_DEVICES made major_in_use() open "" and report every
 *     major free, so the collision warning never fired (N4).
 *
 * WHAT IS KEPT is what an X program needs to reach its display and
 * speak the user's language, and PATH - which vlhe_priv_raise()
 * replaces with a fixed one for the raised window (S5).
 *
 * WHAT IS SET: HOME from the password file rather than trusted from
 * the environment (N5 - paths built from it are opened after
 * Modify), and GTK_RC_FILES to the system file alone.
 *
 * THE COST: the setuid build does not read ~/.gtkrc, so a user's own
 * GTK theme does not apply there. That is deliberate - an rc file can
 * name a theme ENGINE, a shared object, and this process keeps a
 * recoverable root in its saved uid (S2). The target has no plain
 * /etc/gtk/gtkrc, only locale variants (gtkrc.ja, gtkrc.ko...), and
 * GTK still finds those from this setting, so CJK font sets work.
 *
 * AND THE TEST OVERRIDES ARE GONE WITH EVERYTHING ELSE - VLHE_CONF
 * redirects the config of a program with a "load a kernel module"
 * button. The setuid build cannot be tested with fixtures; the
 * ordinary build is the one the host tests exercise.
 */
static const char *const keep[] = {
    "DISPLAY", "XAUTHORITY", "USER", "LOGNAME", "TERM", "TZ", "PATH",
    "LANG", "LC_ALL", "LC_CTYPE", "LC_MESSAGES", "LC_NUMERIC",
    "LC_COLLATE", "LC_TIME", "LC_MONETARY",
    NULL
};

#define KEEP_MAX  24
#define KEEP_LEN  1100

/*
 * GIVE UP ROOT, OR STOP - the review of design/48's fixes, 2026-09-30.
 *
 * Every drop in this file used to ignore seteuid()'s result. If one
 * ever failed, the process would carry on as root while every other
 * part of it believed it had dropped - which is the one outcome the
 * whole mechanism exists to prevent, arrived at silently.
 *
 * THE REVIEW SUGGESTED STOPPING THE PRIVILEGED ACTIONS FOR THE
 * SESSION. That does not help: the process would still BE root, and
 * greyed buttons would not change what GTK, the polls and Render run
 * as. So it exits.
 *
 * IT SHOULD NEVER FIRE. Dropping to the real uid is always permitted
 * on Linux. The check is for the case nobody expects, which is why it
 * also confirms the result with geteuid() rather than trusting the
 * return value alone.
 */
static void
drop_or_die(void)
{
    /*
     * TWO WAYS TO FAIL, EACH SAID IN ITS OWN WORDS. When seteuid()
     * itself fails, errno says why. When it returns success and
     * geteuid() disagrees, errno is whatever it happened to hold, so
     * printing it would be a stale reason - that case names the uids
     * instead.
     */
    if (seteuid(getuid()) != 0) {
        fprintf(stderr, "vlhe.gtk: could not give up root privilege"
                        " (%s) - stopping rather than carry on as"
                        " root\n", strerror(errno));
        _exit(1);
    }
    if (geteuid() != getuid()) {
        fprintf(stderr, "vlhe.gtk: gave up root privilege but the"
                        " effective uid is still %ld, not %ld -"
                        " stopping rather than carry on as root\n",
                (long) geteuid(), (long) getuid());
        _exit(1);
    }
}

void
vlhe_priv_init(void)
{
    static char kept[KEEP_MAX][KEEP_LEN];
    static char home[KEEP_LEN];
    static char rc_files[] = "GTK_RC_FILES=/etc/gtk/gtkrc";
    struct passwd *pw;
    int i, n = 0;

    /*
     * REBUILD FIRST, DROP SECOND. Between exec and the drop we are
     * still euid 0, and anything that reads the environment in that
     * window reads it privileged. Nothing here does - but the order
     * costs nothing and does not depend on that staying true.
     *
     * COPIED OUT BEFORE clearenv(), which may free the strings
     * getenv() returned. A value too long for the buffer is DROPPED
     * rather than truncated - a truncated DISPLAY or PATH is a wrong
     * one, and absent is the safe failure.
     */
    for (i = 0; keep[i] != NULL && n < KEEP_MAX; i++) {
        const char *v = getenv(keep[i]);

        if (v == NULL || strlen(keep[i]) + strlen(v) + 2 > KEEP_LEN)
            continue;
        sprintf(kept[n++], "%s=%s", keep[i], v);
    }

    clearenv();

    for (i = 0; i < n; i++)
        putenv(kept[i]);

    pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_dir != NULL
        && strlen(pw->pw_dir) + 6 < KEEP_LEN) {
        sprintf(home, "HOME=%s", pw->pw_dir);
        putenv(home);
    }

    putenv(rc_files);

    if (getuid() != 0) {
        /*
         * DROP EFFECTIVE PRIVILEGE, KEEPING THE SAVED SET UID so it
         * can be taken back - which is the whole mechanism and the
         * reason this must be a setuid binary rather than a program
         * that calls seteuid(0) hopefully. An ordinary process
         * cannot get root back; one whose SAVED set-user-ID is 0
         * can.
         */
        drop_or_die();
        g_dropped = 1;
    }
}

/*
 * A CLI SUBCOMMAND KEEPS NO WAY BACK TO ROOT - design/55 R7, design/48
 * S2, design/54 D49, 2026-10-04.
 *
 * vlhe_priv_init() drops only the EFFECTIVE uid and keeps the saved one
 * at 0, so the GUI can take root back after Modify. A subcommand has no
 * Modify - nothing in vlhe_cli.c can unlock, so its raises always fail
 * for a user - yet it ran its whole course holding saved uid 0 and the
 * permitted capabilities: parked, but there for anything that went
 * wrong in it.
 *
 * Dropped for good here. setuid() run by a user only changes the
 * effective uid, so root is taken back for one call first - seteuid(0),
 * then setuid(getuid()) sets real, effective AND saved, and 2.2 clears
 * the capabilities with them - and the result is proved: the uids are
 * the user's and seteuid(0) now FAILS. If the first seteuid(0) fails,
 * there was no saved root to drop (a copy that is not installed
 * setuid), which is the wanted state already.
 *
 * Root running it, and the ordinary build, have nothing held back.
 */
void
vlhe_priv_drop_for_good(void)
{
    uid_t u = getuid();

    if (!g_dropped || u == 0)
        return;
#ifdef VLHE_FAKE_PRIV
    return;                     /* the fake build is never setuid */
#endif
    if (seteuid(0) != 0)
        return;                 /* no saved root: nothing to drop */
    if (setuid(u) != 0) {
        fprintf(stderr, "vlhe: could not give up root privilege for"
                        " good (%s) - stopping\n", strerror(errno));
        _exit(1);
    }
    if (getuid() != u || geteuid() != u || seteuid(0) == 0) {
        fprintf(stderr, "vlhe: root privilege is still within reach"
                        " after giving it up - stopping\n");
        _exit(1);
    }
    g_unlocked = 0;
}

int
vlhe_priv_can_unlock(void)
{
    /* ROOT NEVER SEES THE BUTTON - kcontrol's showSUAButton is
     * called with TRUE and nothing else, only inside the
     * `getuid() != 0' branch. Nor does a session that has already
     * unlocked. */
    return g_dropped && !g_unlocked
           && vlhe_priv_checkpass_path() != NULL;
}

int
vlhe_priv_unlocked(void)
{
    return !g_dropped || g_unlocked;
}

const char *
vlhe_priv_checkpass_path(void)
{
    static const char *const where[] = {
        "/usr/X11R6/bin/kcheckpass",    /* KDE 1.1, where Corel puts it */
        "/usr/bin/kcheckpass",
        "/opt/kde/bin/kcheckpass",
        NULL
    };
    struct stat st;
    int i;

#ifdef VLHE_FAKE_PRIV
    return "(fake kcheckpass)";        /* so Modify is offered - see unlock */
#endif
    for (i = 0; where[i] != NULL; i++)
        if (stat(where[i], &st) == 0 && S_ISREG(st.st_mode))
            return where[i];
    return NULL;
}

int
vlhe_priv_unlock(const char *password, char *why, int max)
{
    const char *kcp;
    int   fd[2];
    pid_t pid;
    int   st;

    if (why != NULL && max > 0)
        why[0] = '\0';

    if (!g_dropped) {
        if (why != NULL)
            strncpy(why, "already running with full privilege", max - 1);
        return 0;               /* nothing to unlock */
    }

#ifdef VLHE_FAKE_PRIV
    /*
     * THE FAKE SETUID GUI - vlhe.gtk.fake.su, for looking at the Modify
     * flow on the workstation, where there is no kcheckpass and no
     * password worth checking. Any non-empty password unlocks; nothing
     * is raised, since the fake backend writes nothing. ONLY THE FAKE
     * BUILD DEFINES THIS; vlhe.gtk.su never sees it (Makefile).
     */
    if (password == NULL || *password == '\0') {
        if (why != NULL)
            strncpy(why, "(fake) a password is needed - any will do",
                    max - 1);
        return VLHE_UNLOCK_WRONG;   /* retryable, as the real one is */
    }
    g_unlocked = 1;
    return 0;
#endif

    kcp = vlhe_priv_checkpass_path();
    if (kcp == NULL) {
        if (why != NULL)
            strncpy(why, "kcheckpass is not installed - start the"
                         " control centre as root instead", max - 1);
        return -1;
    }

    if (password == NULL)
        password = "";

    if (pipe(fd) != 0) {
        if (why != NULL)
            strncpy(why, "could not create a pipe", max - 1);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        if (why != NULL)
            strncpy(why, "could not start kcheckpass", max - 1);
        return -1;
    }

    if (pid == 0) {
        /*
         * THE PIPE BECOMES STDIN, and that is what selects the
         * behaviour we want: kcheckpass uses getpass() when stdin is
         * a TTY and a plain read() when it is not (kcheckpass.cpp:
         * 203-213), so a pipe means no prompt on our terminal and
         * nothing echoed.
         */
        close(fd[1]);
        if (dup2(fd[0], 0) < 0)
            _exit(10);
        close(fd[0]);
        execl(kcp, "kcheckpass", "-s", (char *) 0);
        _exit(10);
    }

    close(fd[0]);
    /*
     * NO NEWLINE, and the close is what ENDS THE READ - kcheckpass
     * reads until EOF, so a pipe left open hangs it.
     */
    if (password[0] != '\0')
        (void) write(fd[1], password, strlen(password));
    close(fd[1]);

    /* GUARDED ON pid > 0 even though fork() just returned it -
     * CLAUDE.md records kill(-1) taking the user's X session twice,
     * and waitpid shares the sign convention. */
    if (pid <= 0 || waitpid(pid, &st, 0) != pid
        || !WIFEXITED(st)) {
        if (why != NULL)
            strncpy(why, "kcheckpass did not run", max - 1);
        return -1;
    }

    /*
     * ITS EXIT CODES, from kcheckpass.cpp:241-247. They are worth
     * telling apart: a wrong password is the user's problem and the
     * others are the machine's.
     */
    switch (WEXITSTATUS(st)) {
    case 0:
        break;                  /* correct */
    case 1:
        if (why != NULL)
            strncpy(why, "that is not the root password", max - 1);
        return VLHE_UNLOCK_WRONG;   /* retryable - the only one */
    case 2:
        if (why != NULL)
            strncpy(why, "kcheckpass cannot read the password"
                         " database", max - 1);
        return -1;
    default:
        if (why != NULL)
            strncpy(why, "kcheckpass failed", max - 1);
        return -1;
    }

    if (seteuid(0) != 0) {
        if (why != NULL)
            strncpy(why, "the password was right but privilege could"
                         " not be regained", max - 1);
        return -1;
    }

    /*
     * AND STRAIGHT BACK DOWN - design/48 S4, 2026-09-30.
     *
     * This used to leave euid 0 for the rest of the session, so
     * everything the program did afterwards ran as root: GTK, the
     * polls, Render, LAME. The regain above proves the password was
     * worth something; the privilege itself is now taken only around
     * the work that needs it, by vlhe_priv_raise() through the
     * backend's vlhe_root_begin().
     *
     * THE USER SEES NO DIFFERENCE - one password at Modify, then
     * Load, Unload and Apply as often as they like. The saved uid
     * stays 0, which is what lets raise work without asking again
     * and is the part of design/48 S2 this does NOT remove.
     */
    drop_or_die();

    g_unlocked = 1;
    return 0;
}

/*
 * A FIXED PATH WHILE RAISED - design/48 S5.
 *
 * Plan steps name insmod, modprobe and rmmod bare, and the
 * diagnostics run dmesg and cat; execvp() searches PATH, and PATH
 * came from whoever launched the program. As root that means a
 * directory the invoking user chose.
 *
 * SWAPPED IN ONLY FOR THE RAISED WINDOW, rather than set once at
 * startup, because the unprivileged work is entitled to the user's
 * own PATH - Render finds a `lame' in ~/bin through it, and that runs
 * as the user now. Every directory here is root's.
 */
static char g_fixed_path[] =
    "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/sbin:/usr/local/bin";
static char g_user_path[4096];  /* "PATH=..." to put back           */
static int  g_had_path;

static void
path_fix(void)
{
    const char *v = getenv("PATH");

    g_had_path = (v != NULL);
    if (g_had_path)
        sprintf(g_user_path, "PATH=%.4000s", v);
    putenv(g_fixed_path);
}

static void
path_restore(void)
{
    if (g_had_path)
        putenv(g_user_path);
    else
        unsetenv("PATH");       /* as it was: absent, not empty -
                                 * an empty PATH means the cwd */
}

int
vlhe_priv_raise(void)
{
    if (!g_unlocked)
        return 0;               /* nothing held, nothing to raise */
    if (g_depth++ == 0) {
        if (seteuid(0) != 0) {
            g_depth--;
            return -1;
        }
        path_fix();
    }
    return 0;
}

void
vlhe_priv_lower(void)
{
    if (!g_unlocked || g_depth == 0)
        return;
    if (--g_depth == 0) {
        path_restore();
        drop_or_die();
    }
}

int
vlhe_priv_suspend(void)
{
    int held = g_depth;

    if (!g_unlocked || held == 0)
        return 0;
    g_depth = 0;
    path_restore();
    drop_or_die();
    return held;
}

int
vlhe_priv_resume(int depth)
{
    if (!g_unlocked || depth <= 0 || g_depth != 0)
        return 0;
    if (seteuid(0) != 0)
        return -1;
    path_fix();
    g_depth = depth;
    return 0;
}

int
vlhe_priv_can_act(void)
{
    return geteuid() == 0 || g_unlocked;
}

/*
 * THE COMMAND LINE REACHES gtk_init() TOO - design/49 N1.
 *
 * The allow-list above covers the environment. GTK 1.2.8 also reads
 * `--gtk-module PATH' and `--gtk-module=PATH' from argv
 * (gtkmain.c:238-252) and g_module_open()s the path (:350): a shared
 * object of the invoker's choosing, loaded into a process that keeps
 * a recoverable root in its saved uid.
 *
 * STRIPPED: every --gtk-*, --gdk-* and --g-* argument - the module
 * option and the debug switches beside it, none of which a user of
 * the setuid build needs - and vlhe.gtk's own -F and its font name
 * (N2), a development preview that pastes text into GTK rc. The BARE `--gtk-module' takes the NEXT
 * argument as its path, so that goes too. Display, sync, name and
 * class are left alone: they steer the X connection, which the
 * allow-listed DISPLAY already lets the user choose.
 *
 * EVERY POSITION IS SCANNED, not only up to `--': GTK does not stop
 * there either, so neither may this.
 */
void
vlhe_priv_filter_argv(int *argc, char **argv)
{
    int i, o;

    if (argc == NULL || argv == NULL)
        return;

    for (i = o = 1; i < *argc; i++) {
        const char *a = argv[i];

        if (a != NULL && strcmp(a, "--gtk-module") == 0) {
            i++;                /* and its path */
            continue;
        }
        if (a != NULL && (strncmp(a, "--gtk-", 6) == 0
                          || strncmp(a, "--gdk-", 6) == 0
                          || strncmp(a, "--g-", 4) == 0))
            continue;
        /*
         * AND OUR OWN -F, WITH ITS NAME - design/49 N2. It pastes a
         * font into GTK rc text, it exists to preview a font during
         * development, and its own comment says it is not meant to
         * ship. Dropped here rather than by an #ifdef in vlhe_cc.c,
         * which is kept free of them.
         */
        if (a != NULL && strcmp(a, "-F") == 0) {
            i++;
            continue;
        }
        argv[o++] = argv[i];
    }
    argv[o] = NULL;
    *argc = o;
}

#else   /* !VLHE_SETUID - the ordinary build */

/*
 * EVERY ONE A NO-OP, so the GUI has one code path.
 *
 * `vlhe_priv_unlocked()' answers TRUE because in this build there is
 * nothing held back: whatever the process can do, it can do now. The
 * controls are then greyed by vlhe_can_administer(), which asks
 * whether the config is WRITABLE rather than whether we are root -
 * design/33 section 2, and the right question for a machine where an
 * admin group owns the file.
 */

void vlhe_priv_init(void)
{
}

void vlhe_priv_drop_for_good(void)
{
}

int vlhe_priv_can_unlock(void)
{
    return 0;                   /* no button, nothing to unlock */
}

int vlhe_priv_unlocked(void)
{
    return 1;
}

int vlhe_priv_unlock(const char *password, char *why, int max)
{
    (void)password;
    if (why != NULL && max > 0)
        strncpy(why, "this build cannot change privilege", max - 1);
    return -1;
}

const char *vlhe_priv_checkpass_path(void)
{
    return NULL;
}

int  vlhe_priv_raise(void) { return 0; }
void vlhe_priv_lower(void) { }
int  vlhe_priv_suspend(void) { return 0; }
int  vlhe_priv_resume(int depth) { (void) depth; return 0; }

/* THE ORDINARY BUILD LEAVES argv ALONE - --gtk-module there loads a
 * module into an unprivileged process, which is GTK working as
 * designed. */
void vlhe_priv_filter_argv(int *argc, char **argv)
{
    (void)argc;
    (void)argv;
}

/* THE ORDINARY BUILD CAN ACT EXACTLY WHEN IT IS ROOT - it has nothing
 * held back to raise. */
int vlhe_priv_can_act(void)
{
    return geteuid() == 0;
}

#endif  /* VLHE_SETUID */
