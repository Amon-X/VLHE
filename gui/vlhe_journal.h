/*
 * vlhe_journal.h - every change made to the machine, and its undo.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHAT THIS IS FOR, in the user's words (2026-09-21):
 *
 *   "The machine for the test state should be in the exact condition
 *    it was and a log showing any nodes created and destroyed what
 *    was loaded what order and the unload. Nothing verbose no
 *    logging. Just a list of all the changes made and changes undid.
 *    So a user can audit and see the changes and if something messed
 *    up this log would show."
 *
 * SO IT IS A CHANGE JOURNAL, NOT A LOG, and the difference decides
 * everything else here. A log records what a program DID; this
 * records what the MACHINE now carries that it did not carry before.
 * Nothing goes in it that did not change state:
 *
 *   IN     a module loaded, a node created or replaced, a daemon
 *          started, a node removed, a module unloaded
 *   OUT    "reading config", "probing card", "3500 writes", timings,
 *          progress, anything a second run would print identically
 *
 * THE TEST IT HAS TO PASS: after `apply' then `apply -u', the journal
 * shows every line matched by its undo and says the machine is as it
 * was. If a line is unmatched, THAT is the finding - it names exactly
 * what was left behind.
 *
 * WHY AN UNDONE ENTRY STAYS. A node that could not be removed or a
 * module that would not rmmod is the single most important thing this
 * file can tell anyone, and clearing it on a failed undo would erase
 * precisely that. So the entry remains marked NOT UNDONE, the next
 * undo retries it, and the machine's outstanding state is always
 * readable in one place.
 *
 * APPENDED, NEVER OVERWRITTEN - the user's call, and the reason is
 * testing: "if someone runs multiple tests each test would overwrite
 * the log they could have tried multiple majors or minors and things
 * could get missed in the loading and unloading". A per-run file
 * destroys the evidence in exactly the case the journal exists for.
 *
 * WHERE IT LIVES FOLLOWS WHERE THE STATE LIVES:
 *
 *   installed     /var/log/vlhe/changes
 *   trial mode    ./vlhe-changes, beside whatever was run
 *
 * A trial run is meant to leave the machine untouched, so it must not
 * write to /var either. VLHE_MODULE_DIR is the trial-mode signal, the
 * same one module_dir() uses.
 *
 * /var/log/vlhe/ IS ALSO THE PLACE FOR ANY OTHER LOGGING WE ADD,
 * following Corel's own arrangement - its /var/log holds per-facility
 * directories (kern/kern.debug, daemon/daemon.err) rather than the
 * flat modern layout. The daemons' output belongs there too; this
 * file is only the journal half.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_JOURNAL_H
#define VLHE_JOURNAL_H

/* What changed. The verb is what was DONE, not what was asked. */
enum vlhe_change {
    VLHE_CH_MODULE = 1,     /* insmod / modprobe                     */
    VLHE_CH_NODE,           /* mknod                                 */
    VLHE_CH_DAEMON,         /* started                               */
    VLHE_CH_RMNODE,         /* a node REMOVED - a change like any    */
    VLHE_CH_RMMODULE,       /* rmmod, incl. moving a card aside      */
    VLHE_CH_DAEMON_STOP     /* stopped - only when it WAS running    */
};

/*
 * OPEN THE JOURNAL FOR ONE RUN. `undo' marks the run as a teardown,
 * which changes only the header - the entries are the same shape
 * either way, because "removed /dev/vdisc0" is a change whether it
 * happens during a load or an unload.
 *
 * Returns 0, or -1 when the file cannot be opened. A run whose
 * journal cannot be written STILL PROCEEDS: refusing to load modules
 * because a log is unwritable would be the tail wagging the dog. The
 * caller reports it and carries on.
 */
int  vlhe_journal_begin(int undo);

/* What vlhe_journal_begin() takes: 0 an apply, 1 an undo, and
 * VLHE_JOURNAL_REPAIR a fix chosen from what needs attention (the
 * Status page's Review..., `vlhe repair') - headed REPAIR and closed
 * with its own line, so it never reads as an unload (2026-10-04). */
#define VLHE_JOURNAL_REPAIR 2

/*
 * RECORD ONE CHANGE, AFTER IT SUCCEEDED.
 *
 * NEVER BEFORE. A journal that lists what was attempted is a journal
 * that lies about the machine's state, and this file's whole value is
 * that a reader can trust every line names something real.
 *
 * `what' is the thing changed - a module name, a node path, a daemon
 * name. `detail' is optional and carries what an auditor would need
 * to reverse it by hand: the major and minor for a node, the
 * arguments for a module. NULL when there is nothing to add.
 */
void vlhe_journal_add(int kind, const char *what, const char *detail);

/*
 * RECORD A CHANGE THAT COULD NOT BE UNDONE. Same fields, plus why.
 * These are the lines that matter most - see the header.
 */
void vlhe_journal_failed(int kind, const char *what, const char *why);

/*
 * CLOSE THE RUN. Writes the one-line verdict: whether the machine is
 * back as it was, or what is outstanding. Safe to call when begin()
 * failed.
 */
void vlhe_journal_end(void);

/*
 * HOW MANY SESSION RECORDS ARE STILL IN EFFECT after an unload -
 * design/54 D23, 2026-10-03. The verdict used to count only what this
 * run attempted, and said "the machine is as it was" over a session
 * file with 21 records still OPEN. The unload sets this before the run
 * closes: 0, the session was complete and filed; N > 0, N records
 * still in effect (still loaded, or not put back); -1, the file could
 * not be read. Not set - a load, or an unload that stopped early -
 * keeps the old wording. Reset by begin().
 */
void vlhe_journal_outstanding(int n);

/*
 * A CHANGE LEFT AS SOMEONE ELSE MADE IT - design/54 7h (CONFLICT,
 * 2026-10-03). Not a failure of VLHE's: the path was changed while VLHE
 * was loaded, and the undo did not fight it. Marked so it is seen, and
 * counted, so the verdict cannot say "as it was".
 */
void vlhe_journal_conflict(int kind, const char *what, const char *why);
/* How many conflicts the run now open has journalled - for a caller
 * that reports them elsewhere (the boot's one-time notice). */
int  vlhe_journal_conflicts(void);
/* 1 while a begin() is nested inside an open run - one scope of a press
 * that runs several plans - so a caller can leave a whole-press report
 * to the outermost level (vlhe_apply_session_close()). */
int  vlhe_journal_nested(void);
/* What vlhe_journal_outstanding() was last told in the run now open:
 * -2 not set, -1 unreadable, else the count - so the GUI's summary can
 * give the same outcome as the journal's verdict. */
int  vlhe_journal_outstanding_now(void);
/* The verdict's second line - design/54 7h Stage 3.4: `n' paths differ
 * from the baseline after the undo, `list' names them (0 = as it was). */
void vlhe_journal_baseline(int n, const char *list);
/* ...and what it was last told: -2 not compared, else the count. Kept
 * after end(), until the next run begins, so the GUI's summary can say
 * the same as the journal (2026-10-04: the box said "as it was" under
 * its own "differs from the baseline" lines). */
int  vlhe_journal_baseline_now(void);
/* Which run is open: a number that changes at every outermost begin(),
 * so a step said once per press can tell a later scope of the same
 * press from the next press. 0 before the first run. */
int  vlhe_journal_run_id(void);

/* Where it is writing, for a caller that wants to name the file.
 * Valid after begin(); a static buffer. */
const char *vlhe_journal_path(void);

/*
 * WHERE THE DAEMONS' OUTPUT GOES - beside the journal, so one
 * directory holds both what changed and what the daemons then said
 * about it.
 *
 *   installed     /var/log/vlhe/DAEMON.LOG
 *   trial mode    ./DAEMON.LOG
 *
 * THE NAME IS load.sh's, deliberately. That script has always
 * redirected its daemons into `DAEMON.LOG' (load.sh:947) and every
 * capture in tests/logs/ carries one, so readlog.sh, the run records
 * and anyone reading them already know what the file is. A second
 * name for the same thing would be a small cruelty.
 *
 * IT LIVES HERE RATHER THAN IN vlhe_apply.c because the trial-mode
 * test is the journal's - one place decides where a run's output
 * goes, and the two files cannot end up in different directories.
 *
 * Returns a static buffer; safe to call before begin().
 */
const char *vlhe_daemon_log_path(void);

#endif /* VLHE_JOURNAL_H */
