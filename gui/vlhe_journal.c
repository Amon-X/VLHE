/*
 * vlhe_journal.c - the change journal. Read vlhe_journal.h first.
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
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "vlhe_journal.h"
#include "vlhe_self.h"

static FILE *g_fp;
static int   g_nchange;         /* lines written this run            */
static int   g_nfailed;         /* of those, ones that did not undo  */
/* Session records still in effect after an unload - -2 not set, -1
 * unreadable, else the count. vlhe_journal_outstanding(). */
static int g_outstanding = -2;
/* Changes left as someone else made them - vlhe_journal_conflict(). */
static int g_nconflict;
/* THE VERDICT'S SECOND LINE - design/54 7h Stage 3.4: paths that differ
 * from the baseline after the undo. -2 not compared, else the count;
 * g_base_list names them. Reset by begin(). */
static int  g_base_diff = -2;
static char g_base_list[400];
/* NESTED begin()s OUTSTANDING. The GUI runs one scoped plan per
 * component, so one press opens this three times and must not write
 * three headers - see vlhe_journal_begin(). */
static int   g_depth;
static int   g_undo;
static int   g_run_id;      /* moves at every outermost begin() */
static int   g_repair;     /* VLHE_JOURNAL_REPAIR: its own heading and verdict */
static char  g_path[256];

/*
 * WHERE THE JOURNAL GOES, AND IT FOLLOWS WHERE THE STATE GOES.
 *
 * A trial run is meant to leave the machine as it found it, so it
 * writes beside whatever was run rather than into /var - the same
 * reasoning that keeps trial-mode modules out of /lib/modules.
 * VLHE_MODULE_DIR is the signal, matching module_dir() in
 * vlhe_apply.c rather than inventing a second test for the same
 * thing.
 *
 * VLHE_JOURNAL overrides both, for the host tests.
 */
static const char *
journal_path(void)
{
    const char *env;

    env = getenv("VLHE_JOURNAL");
    if (env != NULL && *env != '\0') {
        strncpy(g_path, env, sizeof g_path - 1);
        g_path[sizeof g_path - 1] = '\0';
        return g_path;
    }

    /*
     * TRIAL MODE: BESIDE THE MODULES, NAMED ABSOLUTELY.
     *
     * IT USED TO BE "./vlhe-changes" AND THAT WAS A REAL BUG, found
     * 2026-09-22. `.' is the PROCESS'S working directory, not the
     * trial directory - the same thing only when a user runs
     * ./vlhe.gtk from the folder they unpacked.
     *
     * An init script's cwd is `/'. So
     *
     *     VLHE_DIR=/mnt/xfer /etc/init.d/vlhe start
     *
     * exported VLHE_MODULE_DIR=/mnt/xfer, took this branch, and wrote
     * the journal to `./vlhe-changes' relative to wherever the user
     * happened to be standing - /root, in the run that found this.
     * The eleven changes went there, /var/log/vlhe/changes kept an
     * older entry from a run without VLHE_DIR, and the failing `stop'
     * then pointed the user at a file that had nothing to do with it.
     *
     * THE DIRECTORY IS ALREADY IN HAND, so there is no reason to
     * guess at it with a relative path.
     */
    env = getenv("VLHE_MODULE_DIR");
    if (env != NULL && *env != '\0') {
        if (strlen(env) + sizeof "/vlhe-changes" <= sizeof g_path) {
            strcpy(g_path, env);
            strcat(g_path, "/vlhe-changes");
        } else {
            /* Absurdly long - fall back rather than truncate into
             * some other directory's file. */
            strcpy(g_path, "/var/log/vlhe/changes");
        }
        return g_path;
    }

    /*
     * A PORTABLE COPY KEEPS ITS JOURNAL IN ITS OWN FOLDER, beside the
     * binary, and finds it there however it was launched.
     *
     * VLHE_MODULE_DIR ABOVE IS NOT ENOUGH, which is why this exists:
     * only the init script sets it. A user who DOUBLE-CLICKS the GUI
     * sets nothing, and without this the journal would go to
     * /var/log/vlhe/ - a system directory, from a copy that promises
     * to touch nothing of the system, and one an ordinary user cannot
     * write to anyway.
     *
     * The override above still wins, because a caller who named a
     * directory meant it.
     */
    if (vlhe_self_is_trial()
        && vlhe_self_path("vlhe-changes", g_path, sizeof g_path))
        return g_path;

    strcpy(g_path, "/var/log/vlhe/changes");
    return g_path;
}

const char *
vlhe_journal_path(void)
{
    return g_path[0] != '\0' ? g_path : journal_path();
}

/*
 * THE DAEMONS' LOG, in the same directory the journal chose - see
 * vlhe_journal.h for why it lives here and why it is called
 * DAEMON.LOG.
 *
 * Derived from journal_path() rather than repeating its trial-mode
 * test, so the two files cannot disagree about which directory a run
 * writes to.
 */
const char *
vlhe_daemon_log_path(void)
{
    static char path[256];
    const char *j = journal_path();
    const char *slash = strrchr(j, '/');

    if (slash == NULL) {
        strcpy(path, "DAEMON.LOG");
        return path;
    }
    if ((size_t)(slash - j) > sizeof path - 12) {
        /* Cannot happen with either real path; fail somewhere
         * harmless rather than truncate into a wrong directory. */
        strcpy(path, "/dev/null");
        return path;
    }
    memcpy(path, j, (size_t)(slash - j) + 1);
    strcpy(path + (slash - j) + 1, "DAEMON.LOG");
    return path;
}

/* The verb as it appears in the file. Fixed width so the columns line
 * up when someone reads it with `cat' on a 80-column console, which
 * is how it will actually be read. */
static const char *
verb(int kind)
{
    switch (kind) {
    case VLHE_CH_MODULE:    return "load   module";
    case VLHE_CH_NODE:      return "create node  ";
    case VLHE_CH_DAEMON:    return "start  daemon";
    case VLHE_CH_RMNODE:    return "remove node  ";
    case VLHE_CH_RMMODULE:  return "unload module";
    case VLHE_CH_DAEMON_STOP: return "stop   daemon";
    }
    return "?????  ?????";
}

int
vlhe_journal_begin(int undo)
{
    const char *path = journal_path();
    time_t now;
    struct tm *tm;
    char stamp[32];

    /*
     * NESTED CALLS JOIN THE RUN ALREADY OPEN, they do not start a
     * second one - added 2026-09-23, after the scoped plan made the
     * GUI call vlhe_plan_run() three times for one press.
     *
     * WHAT IT LOOKED LIKE. A single Unload wrote THREE headers:
     *
     *     19:48:36  UNDO   (nothing changed)
     *     19:48:36  UNDO   (nothing changed)
     *     19:48:36  UNDO   unload module sb / load module sb ...
     *                      2 change(s) undone - the machine is as it
     *                      was before the apply.
     *
     * Two of them empty, and EACH claiming the machine is as it was -
     * which no single scope can know, since it saw only its own
     * component.
     *
     * COUNTED RATHER THAN FLAGGED, so the matching end() closes it.
     * A caller that begins twice must end twice, which every path
     * here already does.
     */
    if (g_fp != NULL) {
        g_depth++;
        return 0;
    }

    g_run_id++;
    g_nchange = 0;
    g_nfailed = 0;
    g_undo    = undo == 1;
    g_repair  = undo == VLHE_JOURNAL_REPAIR;
    g_outstanding = -2;
    g_nconflict   = 0;
    g_base_diff   = -2;
    g_base_list[0] = '\0';

    /*
     * THE DIRECTORY MAY NOT EXIST on a machine where nothing has been
     * installed. Made here rather than by the packaging, so a trial
     * run out of an extracted tarball works with no setup - which is
     * the whole point of trial mode.
     *
     * The failure is ignored deliberately: if it already exists that
     * is success, and if it cannot be made the fopen below reports
     * the real reason.
     */
    if (strncmp(path, "/var/", 5) == 0) {
        mkdir("/var/log/vlhe", 0755);
    }

    /* NO LINK FOLLOWED - design/55 recommendation 2: in a portable
     * folder this name is in a directory a user may own. */
    g_fp = vlhe_safe_append(path);
    if (g_fp == NULL)
        return -1;

    now = time(NULL);
    tm  = localtime(&now);
    if (tm != NULL)
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", tm);
    else
        strcpy(stamp, "(no clock)");

    fprintf(g_fp, "\n%s  %s\n", stamp,
            g_repair ? "REPAIR" : g_undo ? "UNDO" : "APPLY");
    fflush(g_fp);
    return 0;
}

void
vlhe_journal_add(int kind, const char *what, const char *detail)
{
    if (g_fp == NULL || what == NULL)
        return;

    fprintf(g_fp, "  %s %s", verb(kind), what);
    if (detail != NULL && *detail != '\0')
        fprintf(g_fp, "  %s", detail);
    fputc('\n', g_fp);
    fflush(g_fp);               /* SO A CRASH STILL LEAVES THE RECORD */
    g_nchange++;
}

void
vlhe_journal_failed(int kind, const char *what, const char *why)
{
    if (g_fp == NULL || what == NULL)
        return;

    /*
     * MARKED SO IT CANNOT BE SKIMMED PAST. These are the lines the
     * file exists for - everything else says the machine is fine.
     */
    fprintf(g_fp, "  %s %s  *** NOT DONE: %s\n",
            verb(kind), what, why != NULL ? why : "unknown");
    fflush(g_fp);
    g_nchange++;
    g_nfailed++;
}

int
vlhe_journal_conflicts(void)
{
    return g_nconflict;
}

void
vlhe_journal_conflict(int kind, const char *what, const char *why)
{
    if (g_fp == NULL || what == NULL)
        return;
    fprintf(g_fp, "  %s %s  *** CONFLICT: left as found - %s\n",
            verb(kind), what, why != NULL ? why : "changed while loaded");
    fflush(g_fp);
    g_nchange++;
    g_nconflict++;
}

void
vlhe_journal_outstanding(int n)
{
    g_outstanding = n;
}

int
vlhe_journal_run_id(void)
{
    return g_run_id;
}

int
vlhe_journal_baseline_now(void)
{
    return g_base_diff;
}

void
vlhe_journal_baseline(int n, const char *list)
{
    g_base_diff = n;
    g_base_list[0] = '\0';
    if (list != NULL) {
        strncpy(g_base_list, list, sizeof g_base_list - 1);
        g_base_list[sizeof g_base_list - 1] = '\0';
    }
}

int
vlhe_journal_outstanding_now(void)
{
    return g_outstanding;
}

int
vlhe_journal_nested(void)
{
    return g_fp != NULL && g_depth > 0;
}

void
vlhe_journal_end(void)
{
    if (g_fp == NULL)
        return;

    /* AN INNER end() JUST UNWINDS - the verdict belongs to the
     * outermost one, which is the only level that has seen every
     * change. See vlhe_journal_begin(). */
    if (g_depth > 0) {
        g_depth--;
        return;
    }

    /*
     * THE SESSION FILE HAS THE LAST WORD ON "AS IT WAS" - design/54
     * D23. What this run did is not the question; what the machine
     * still carries is, and the session file is the record of that.
     * Checked FIRST, so a run that changed nothing still says so when
     * an earlier load's records are open.
     */
    /* A REPAIR IS NOT AN UNLOAD - the user, 2026-10-04, reading the
     * journal: each fix had been filed "UNDO ... the machine is as it
     * was before the apply". It says what it was instead. */
    if (g_repair) {
        if (g_nchange == 0 && g_nfailed == 0)
            fprintf(g_fp, "  (nothing changed)\n");
        else if (g_nfailed > 0)
            fprintf(g_fp, "  %d put right, %d NOT DONE - chosen from what"
                          " needed attention.\n", g_nchange, g_nfailed);
        else
            fprintf(g_fp, "  %d put right by choice, from what needed"
                          " attention.\n", g_nchange);
        fclose(g_fp);
        g_fp = NULL;
        return;
    }

    if (g_undo && g_nfailed == 0 && g_outstanding > 0) {
        fprintf(g_fp, "  %d change(s) undone; %d still in effect in the"
                      " session file - the machine is not as it was.\n",
                g_nchange, g_outstanding);
    } else if (g_undo && g_nfailed == 0 && g_nconflict > 0) {
        /* CONFLICTS ARE FINAL BUT NOT "AS IT WAS" - the user's change
         * won, and the verdict says so rather than claiming clean. */
        fprintf(g_fp, "  %d change(s) undone; %d left as someone else"
                      " changed them (CONFLICT, above) - the machine is"
                      " not exactly as it was.\n",
                g_nchange - g_nconflict, g_nconflict);
    } else if (g_undo && g_nfailed == 0 && g_outstanding == -1) {
        fprintf(g_fp, "  %d change(s) undone - the session file could not"
                      " be read, so whether anything is still in effect"
                      " is not known.\n", g_nchange);
    } else if (g_nchange == 0) {
        fprintf(g_fp, "  (nothing changed)\n");
    } else if (g_nfailed > 0) {
        fprintf(g_fp, "  %d change(s) recorded, %d NOT DONE -"
                      " the machine is not as it was.\n",
                g_nchange, g_nfailed);
    } else if (g_undo) {
        fprintf(g_fp, "  %d change(s) undone - the machine is as it"
                      " was before the apply.\n", g_nchange);
    } else {
        fprintf(g_fp, "  %d change(s). `vlhe apply -u' reverses them.\n",
                g_nchange);
    }

    /*
     * AND THE SECOND LINE: HOW THE MACHINE COMPARES WITH THE BASELINE -
     * design/54 7h Stage 3.4. The line above says whether VLHE's own
     * undo was clean; this one says what still differs from how the
     * machine was, so a change the user chose to keep shows here without
     * reading as VLHE failing.
     */
    if (g_undo && g_base_diff == 0)
        fprintf(g_fp, "  Compared with the baseline: as it was.\n");
    else if (g_undo && g_base_diff > 0)
        fprintf(g_fp, "  Compared with the baseline: %d path(s) differ -"
                      " %s.\n", g_base_diff, g_base_list);

    fclose(g_fp);
    g_fp = NULL;
}
