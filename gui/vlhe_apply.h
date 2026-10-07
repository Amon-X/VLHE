/*
 * vlhe_apply.h - turn the config on disk into commands.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * THE PIECE THAT WAS MISSING. design/33 section 1d: "Nothing
 * currently reads a config and acts on it. The GUI writes
 * /etc/vlhe.conf and the daemons read no config at all - they take
 * compiled-in defaults and command-line flags. So a setting written
 * today reaches a running system only if something translates it
 * into a command line, and that something does not exist."
 *
 * This is that translation, and ONLY that translation.
 *
 * IT DOES NOT RUN ANYTHING. Building the command list and executing
 * it are deliberately separate, for two reasons:
 *
 *   1. THE THINKING IS HERE AND IT IS TESTABLE. Which module loads
 *      first, which parameter goes where, what the daemons' flags
 *      are - all of that is a pure function of the config, and a
 *      host test can check every line of it without touching a
 *      device. The executing half is then a thin loop with no
 *      decisions left in it.
 *   2. `vlhe apply -n' IS A REAL FEATURE, not a testing aid. Someone
 *      who hand-edits /etc/vlhe.conf wants to see what it means
 *      before it means it.
 *
 * C89, GCC 2.95.2, no toolkit - it links into vlhe, which exists
 * precisely so a machine with no X can still be configured.
 */

#ifndef VLHE_APPLY_H
#define VLHE_APPLY_H

#include <stdio.h>

/* A command's length. `insmod vsound.o dsp=3 rate=44100 chans=4' and
 * the vmidid line with a font path are the long ones.
 *
 * 1024 SINCE 2026-10-02, was 512. The directory in front of a daemon
 * or module is now carried whole (up to 255 bytes) instead of cut at
 * 120, and the vmidid step's `@FONTn@' tags expand at run time into
 * full soundfont paths in a buffer of this size - two of those plus a
 * long directory would not have fitted in 512. A plan is 32 steps, so
 * this costs 16 KB per plan. */
#define VLHE_CMD_MAX    1024

/*
 * HOW MANY `@FONTn@' TAGS A STEP MAY CARRY - design/36 row 91.
 *
 * A step holds `-s @FONT0@' rather than the path, and `expand_nodes()'
 * substitutes the real one at run time, the way it already does for
 * `@VSOUND@' and `@CARD@'. That keeps a 150-character soundfont path
 * out of a buffer that exists to describe the plan.
 *
 * EIGHT BECAUSE THAT IS WHAT THE SYNTH TAKES - `RENDER_MAX_FONTS' in
 * `vmidi/synth/render.h', and `VLHE_MAX_FONTS' matches it. The tag is
 * a single digit, so this must stay under ten or the parser below
 * needs widening.
 */
#define VLHE_FONT_TAGS  8

/* HOW MANY. Four modules, four daemons, and the unload side of each,
 * with room to grow. A fixed array rather than a list: the count is
 * bounded by the design, and C89 without malloc is one less thing to
 * get wrong in a program that runs as root. */
#define VLHE_PLAN_MAX   32

/*
 * WHAT EACH STEP IS FOR, so a dry run can explain itself and a
 * failure can say which stage broke rather than only which command.
 */
enum vlhe_step_kind {
    VLHE_STEP_MODULE,       /* insmod / rmmod                        */
    VLHE_STEP_DAEMON,       /* vsoundd, vmidid, vdiscd               */
    VLHE_STEP_NODE,         /* mknod for a device node that is absent */
    VLHE_STEP_CHECK         /* a precondition, not a change           */
};

struct vlhe_step {
    int  kind;                      /* enum vlhe_step_kind           */
    char cmd[VLHE_CMD_MAX];         /* the command, as it would run  */
    char why[128];                  /* one line, for -n and for logs */
    int  optional;                  /* a failure here is not fatal   */
    int  comp;                      /* VLHE_ENABLE_* the step is for,
                                     * or 0 for a plan-wide one - the
                                     * simulation's section lines     */
    /*
     * RUN THIS EVEN IF AN EARLIER STEP FAILED.
     *
     * THE INVARIANT IT EXISTS FOR, the user's words 2026-09-23: "if
     * we rmmod the users card it MUST be restored on unload or a
     * cleanup on failure path too. If vsound fails to load sound
     * must be working even if it is just the users module."
     *
     * WHAT WENT WRONG WITHOUT IT. plan_load() rmmods the card to
     * free device 0, then insmods vsound.o, then modprobes the card
     * back onto device 1. The insmod is NOT optional, so a failure
     * there returned from vlhe_plan_run() immediately - and the
     * restore never ran. The machine was left with its sound card
     * removed and no vsound either: WORSE than before the apply, and
     * worse than either component failing on its own.
     *
     * THE RESTORE STEP'S OWN COMMENT ALREADY SAID SO - "it is not
     * optional to the USER even though the step is marked so" - and
     * load.sh records the day a machine was "left with the card
     * rmmod'd and NOTHING loaded". The risk was written down twice
     * and enforced nowhere; this is the enforcement.
     *
     * A CLEANUP STEP IS NOT AN OPTIONAL STEP. `optional' says a
     * failure may be stepped over on the way FORWARD. This says the
     * step must happen on the way OUT - the two are independent, and
     * the restore is both: it may fail (the card may not come back)
     * and it must be attempted (or the machine loses its sound).
     *
     * IT IS A PROPERTY OF THE STEP, NOT A CASE IN THE RUNNER. The
     * runner still has no idea what a sound card is, which is what
     * vlhe_apply.c's header means by "a loop with NO decisions in
     * it".
     */
    int  cleanup;                   /* runs even after a failure     */
};

/*
 * WHICH COPY OF VLHE IS IN USE - installed, or an unpacked folder.
 *
 * THE USER'S SCENARIO, 2026-09-21: "a user downloads the prebuilt
 * stuff unzips it runs the gui. gui detects no config nothing
 * installed and goes into trial mode. A user can try it without
 * installing without a config written to."
 *
 * SO IT IS DETECTED, NOT A FLAG. A user who unzips an archive and
 * double-clicks does not know a flag exists, and requiring one makes
 * the easy path the broken one.
 *
 * THE PORTABLE TREE WINS WHEN BOTH EXIST - REVERSED 2026-10-02.
 * This said "INSTALLED WINS", on the user's reasoning that installed
 * modules "are most likely rebuilt for that machine", with the upgrade
 * case (a newer archive's GUI driving the old installed modules) as
 * the correct outcome. The user reversed that for the CONFIG on
 * 2026-09-22 (vlhe_conf.c: "A PORTABLE TREE IS SELF-CONTAINED") and
 * asked on 2026-10-02 for the modules and daemons to follow: a
 * portable test on a machine with the .deb installed was silently
 * loading the installed modules and starting /usr/sbin's daemons.
 *
 * THE KERNEL RISK IS REAL AND IS NOW THE TREE'S TO CARRY. A portable
 * tree's modules are for the kernel its PORTABLE file names; on any
 * other, insmod refuses (MODVERSIONS) or the user builds from source.
 *
 * `also_folder' now fires only when VLHE_MODULE_DIR overrode a
 * portable tree - the one case left where a folder's copy is present
 * and not used.
 */
enum vlhe_where {
    VLHE_WHERE_NONE = 0,    /* no modules found anywhere            */
    VLHE_WHERE_INSTALLED,   /* /lib/modules/<uname -r>/misc         */
    VLHE_WHERE_FOLDER,      /* beside the binary - TRIAL MODE       */
    VLHE_WHERE_OVERRIDE     /* VLHE_MODULE_DIR - a test, or a user  */
                            /* who named a directory deliberately   */
};

/*
 * Which one, and where. `dir' receives the directory (may be NULL),
 * `also_folder' is set when a portable tree's modules are present
 * but something else won - since 2026-10-02 only VLHE_MODULE_DIR.
 */
int vlhe_where_are_modules(char *dir, int max, int *also_folder);

/* Is this a trial run - modules present but nothing installed?
 * Shorthand for the above, for the callers that only want the flag. */
int vlhe_is_trial(void);

struct vlhe_plan {
    struct vlhe_step step[VLHE_PLAN_MAX];
    int              n;
    int              truncated;     /* more steps than VLHE_PLAN_MAX */
    /*
     * WHICH DIRECTION THIS PLAN GOES. Set by vlhe_plan_build() from
     * its `unload' argument, so the runner does not have to infer it
     * from the steps - which it could only do by guessing at their
     * contents, and would get wrong for a plan that happens to
     * contain an rmmod on the way in (the device-0 contest does).
     *
     * The change journal needs it for its header, and that is the
     * only reader today.
     */
    int              unload;
};

/*
 * BUILD THE PLAN from the config the backend has loaded. Returns the
 * number of steps, or -1 if the config could not be read.
 *
 * `unload' builds the teardown instead - the reverse order, which is
 * not merely the list backwards (daemons must stop before their
 * modules go, and vsound is last out because the others hold it).
 */
/* WHICH SWITCHES A PLAN OBEYS: on = LoadAtBoot (the init script's
 * `vlhe apply'); off, the default = the control centre's Include
 * boxes. The CLI sets it once; the GUI never does. 2026-10-02. */
void vlhe_plan_from_boot_keys(int on);

int vlhe_plan_build(struct vlhe_plan *out, int unload);

/*
 * THE SAME, SCOPED TO ONE COMPONENT.
 *
 * `which' is VLHE_ENABLE_SOUND, _MIDI or _CD, or VLHE_SCOPE_ALL for
 * everything enabled - which is what vlhe_plan_build() passes, so the
 * two are one function and cannot drift.
 *
 * WHY IT EXISTS: THREE SEPARATE ASKS, ONE MISSING PARAMETER. Recorded
 * 2026-09-23, when the plan builder had only a direction:
 *
 *   1. vmidi and vdisc should still load when vsound FAILS. They are
 *      marked optional and would tolerate their own failure, but
 *      vlhe_plan_run() returns at the first REQUIRED failure and they
 *      are never reached.
 *   2. per-row Load and Unload on the Status page (design/32).
 *   3. RELOADING ONE COMPONENT after a settings change - the user's
 *      case: "if you increase the number of drives or decrease them
 *      you just need to unload the daemons and vdisc and then reload
 *      vdisc not the entire stack".
 *
 * SCOPING IS NOT PURELY SUBTRACTIVE, and this is the trap it has to
 * avoid. `load.sh:469' knows it:
 *
 *     if [ -n "$NOVSOUND" ]; then CARDNODE=0; else CARDNODE=1; fi
 *
 * Without vsound the card takes /dev/dsp ITSELF, so a check against
 * dsp1 "would report a working card as broken" - the script's own
 * words. A plan that simply omits vsound's steps is wrong about
 * everything downstream of them.
 *
 * WHAT THE SCOPES DO NOT CHANGE:
 *
 *   - soundcore is loaded by ANY scope. vsound and vmidi both import
 *     from it and it is cheap; leaving it out to be tidy would make
 *     three scopes able to fail where one cannot.
 *   - THE DEVICE-0 CONTEST BELONGS TO SOUND'S SCOPE ALONE. A CD-only
 *     plan must never rmmod the user's card, and a test asserts it.
 *   - vsound is still removed LAST within whatever is in scope, since
 *     vmidi writes into it.
 */
#define VLHE_SCOPE_ALL (-1)

int vlhe_plan_build_scoped(struct vlhe_plan *out, int unload, int which);

/*
 * AS ABOVE, BUT `forced' PLANS THE NAMED COMPONENT EVEN IF ITS
 * Include-in-load BOX IS CLEAR - design/43, 2026-09-26.
 *
 * FOR THE PER-ROW Load BUTTONS ON THE STATUS PAGE, and nothing else.
 * Pressing a button that names one component IS the request to load
 * it; refusing because a standing preference says otherwise would
 * send the user back to the checkbox, which is the thing those
 * buttons exist to stop them having to touch.
 *
 * IT DOES NOT WRITE THE SETTING. The plan runs once; the config is
 * untouched and the next full Load still honours the box.
 *
 * IGNORED for VLHE_SCOPE_ALL and on an unload - see the definition.
 * `vlhe_plan_build_scoped()' is this with `forced' 0.
 */
int vlhe_plan_build_forced(struct vlhe_plan *out, int unload, int which,
                           int forced);

/* Print a plan the way `vlhe apply -n' does. */
void vlhe_plan_print(const struct vlhe_plan *p, FILE *fp);

/*
 * THE SIMULATION - a SHELL SCRIPT: the load half, then the unload half,
 * for the components ticked, every step rendered as the command that
 * does it by hand (a node made, a daemon started or signalled, a link
 * put back), with the run-time names replaced by STATED assumptions
 * (/dev/dsp the card, /dev/dsp1 vsound) and the fonts by the settings'
 * own. `verbose' adds the plan's reason above each line as a comment.
 * Nothing is run, nothing is probed, no root is needed; it assumes
 * every command succeeds and says so. The user's design, 2026-10-01:
 * copy the halves into a load script and an unload script and do it
 * without the GUI. Writes to `fp'; 0 on success.
 */
int vlhe_plan_simulate(FILE *fp, int verbose);

/*
 * RUN A PLAN. The other half of the split this file's header
 * describes - a loop with no decisions left in it.
 *
 * NEVER CALL THIS ON THE WORKSTATION. It insmods modules built for
 * 2.2.16 and starts daemons that open /dev/dsp. CLAUDE.md section 1
 * is explicit and the environment overrides do not help: redirecting
 * a config path does not stop an insmod. The caller must have passed
 * vlhe_apply_machine_ok() first; this checks again anyway, because a
 * guard that is someone else's responsibility is not a guard.
 *
 * STEPS MARKED `optional' MAY FAIL and the run continues - a module
 * already loaded, a daemon already up. A REQUIRED step that fails
 * stops everything: going on after vsound.o failed to load would
 * start a pump with nothing to pump into.
 *
 * ON THE UNLOAD SIDE NOTHING IS REQUIRED, deliberately. Teardown
 * runs to the end even when a piece is missing, because stopping
 * halfway leaves a machine in a state nobody chose.
 *
 * `out' receives a line per step as it runs, so a GUI can show
 * progress; NULL for silence. Returns 0 if every required step
 * succeeded, or the number of the step that stopped it (1-based).
 */
int vlhe_plan_run(const struct vlhe_plan *p, FILE *out);

/*
 * THE KERNEL-LOG CAPTURE - [Tracing] Capture, off by default, and only
 * with [Tracing] Enabled and as root. begin: stop sysklogd, read
 * /proc/kmsg into run-<stamp>/trace.log beside DAEMON.LOG; 1 if
 * started, 0 if not (and why, on `out'). end: stop the reader, copy
 * DAEMON.LOG in, restart sysklogd if begin stopped it; 0 if there was
 * nothing to close, 1 closed, 2 closed but it had ended early. Called
 * around a Load and after an Unload, by the GUI and `vlhe apply'.
 */
int vlhe_capture_begin(FILE *out);
int vlhe_capture_end(FILE *out);

/*
 * IS THIS THE RIGHT MACHINE? design/33 section 1d makes this a
 * REQUIREMENT rather than a courtesy, and it is in the program from
 * the first commit for the reason recorded there: /etc/modules,
 * /etc/rcS.d and /dev/dsp exist on every Linux machine ever made,
 * with identical names and completely different contents. The user's
 * own workstation has an /etc/modules holding nct6775.
 *
 * Two independent signals, both of which must pass:
 *
 *   1. `uname -r' begins 2.2.16
 *   2. /etc/modules mentions kerneld - Corel's stock header documents
 *      auto/noauto as kerneld controls and a modern one does not.
 *      Fails in the SAFE direction: a machine without that word is
 *      not ours.
 *
 * Returns 0 if this machine is a valid target. Otherwise -1, with a
 * one-line reason in `why'.
 *
 * THERE IS NO --force AND THERE MUST NOT BE. A flag that exists will
 * eventually be typed, and there is no legitimate case for applying a
 * 2.2.16 configuration to a machine that is not running 2.2.16.
 */
/*
 * vlhe_apply_set_relink_cb() IS GONE - design/54 7h Stage 1.3,
 * 2026-10-03. It asked, when /dev/dsp had moved since the Load,
 * whether to restore anyway (design/38 9g); nothing ever registered
 * it, so every such restore went ahead. The user's decision 5 replaced
 * it: a link that no longer points where the Load recorded is
 * someone's change, left as found and marked CONFLICT.
 */

/* link_dsp_restore() for the host tests: `made' is the target the Load
 * recorded (empty for an old record). 0 restored, -1 failed, 2
 * CONFLICT - left as found. */
int vlhe_apply_link_dsp_restore(const char *path, const char *saved,
                                const char *made, FILE *out);

/* The unload's two record-driven passes, for the host tests - design/54
 * 7h, the retry: each acts on OPEN and FAILED records and returns how
 * many it acted on (-1 / 0 when the session file cannot be read). */
int vlhe_apply_restore_links(FILE *out);
int vlhe_apply_remove_nodes(int want_midi, FILE *out);

/* The baseline's inference, for the host tests - design/54 7h Stage
 * 3.2. `now' is what is at `path' (vlhe_baseline_describe()); `orig'
 * gets the state to record; returns VLHE_BASE_FOUND / INFERRED /
 * UNKNOWN. Reads the session files, live and filed; touches nothing. */
int vlhe_apply_baseline_infer(const char *path, const char *now, char *orig,
                              size_t max);

/* THE BASELINE, COMPARED AT EVERY LOAD - design/54 7h Stage 3.3. A
 * path that differs from the baseline and is not VLHE's own leftover is
 * DRIFT; the caller's `ask' sees the list and returns one decision. */
struct vlhe_drift {
    char what[VLHE_PATH_MAX];
    char was[VLHE_PATH_MAX];    /* the baseline's state, or "unknown" */
    char now[VLHE_PATH_MAX];
};
/* The answers, in the dialog's words (2026-10-04): KEEP is "Load anyway"
 * (changes left, baseline unchanged, asked again next Load), UPDATE is
 * "Accept changes" (they become the baseline), UNDO is "Undo changes"
 * (put back to the baseline, then load), CANCEL loads nothing. */
enum { VLHE_DRIFT_KEEP = 0, VLHE_DRIFT_UPDATE, VLHE_DRIFT_CANCEL,
       VLHE_DRIFT_UNDO };

/* The CLI's prompt, the GUI's dialog; NULL (the default) warns, keeps
 * the baseline and leaves the one-time notice - the boot's behaviour. */
void vlhe_apply_set_drift_ask(int (*ask)(const struct vlhe_drift *d, int n));

/* Which front end shows this run's messages, so they name only its own
 * way of doing things (the user, 2026-10-04). 0 - none, the boot - is
 * the default. */
enum { VLHE_FRONT_CLI = 1, VLHE_FRONT_GUI };
void vlhe_apply_set_frontend(int which);

/* Run by every Load before its first change (vlhe_plan_run, or the GUI
 * once per press): VLHE's leftovers put back to the baseline, drift
 * decided. 0 go on, -1 cancelled - nothing was changed. */
int vlhe_apply_baseline_check(FILE *out);

/* 1 when the last vlhe_apply_baseline_check() refused the Load because
 * [Load] BaselineDrift is "refuse" - not because someone pressed
 * Cancel - design/54 7h Stage 4. */
int vlhe_apply_drift_refused(void);

/* WHAT NEEDS THE USER'S ATTENTION - design/54 7h Stage 3.5: a dsp link
 * that may be VLHE's leftover and cannot be shown to be, and a
 * /dev/cdrom left pointing at VLHE's own gone target. */
enum { VLHE_FIND_DSP = 1, VLHE_FIND_CDROM };
struct vlhe_finding {
    int  kind;
    char path[VLHE_PATH_MAX];
    char now[VLHE_PATH_MAX];            /* what is there, baseline words */
    char text[2 * VLHE_PATH_MAX + 128]; /* one line, for messages        */
    char was[VLHE_PATH_MAX];            /* /dev/cdrom: the drive the      */
                                        /* baseline knows it pointed at,  */
                                        /* "" when not known (2026-10-04) */
};
/* Up to `max' findings; the count. Reads only - opens a dsp target with
 * O_NONBLOCK only when that cannot load a module (vlhe_modconf.h). */
int vlhe_apply_findings(struct vlhe_finding *f, int max);

/* PUT ONE RIGHT, by the user's choice: VLHE_FIX_STOCK (a dsp path back to
 * MAKEDEV's node), VLHE_FIX_KEEP (a dsp link kept - the baseline only),
 * VLHE_FIX_DRIVE (/dev/cdrom to `arg', a block device), VLHE_FIX_REMOVE
 * (/dev/cdrom removed). The choice becomes the baseline, as FOUND. The
 * three that change /dev pass the machine check, raised, under the run
 * lock. 0 done (or no longer needed), -1 refused or failed. */
enum { VLHE_FIX_STOCK = 1, VLHE_FIX_KEEP, VLHE_FIX_DRIVE, VLHE_FIX_REMOVE };
int vlhe_apply_fix_finding(const char *path, int kind, int action,
                           const char *arg, FILE *out);

/* This machine's CD drives (/dev/hdX, /dev/scdN), for Point at drive. */
int vlhe_apply_cd_drives(char (*list)[VLHE_PATH_MAX], int max);

int vlhe_apply_machine_ok(char *why, size_t len);

/* 1 if a uname release string is a Linux 2.2 ("2.2." and a digit),
 * else 0 - the test vlhe_apply_machine_ok() applies to uname(). */
int vlhe_apply_release_ok(const char *release);

/* 1 if the pam console.perms file at PATH gives <sound> to the console
 * user with no read and write for others (Red Hat 6.0's 0600), else 0 -
 * also 0 for no such file. */
int vlhe_apply_console_sound_locked(const char *path);

/* 1 if a running daemon's arguments (RUNNING: its /proc cmdline, NUL
 * separated, RLEN bytes) differ from the step command EXPANDED, argv[0]
 * aside - the "running with different settings" note. */
int vlhe_apply_args_differ(const char *expanded, const char *running, int rlen);

/* FOR THE HOST TESTS ONLY: a step's command expanded (@CARD@, @FONTn@)
 * and split into argv exactly as run_command() does it, with nothing
 * run. `buf' holds the expanded line; `argv' points into it and gets a
 * NULL after the last word. Returns the word count. design/47 M5. */
/* FOR THE HOST TESTS: render one step as the simulation would, with
 * the stated assumptions and no verbosity. */
void vlhe_apply_sim_step(const struct vlhe_step *s, FILE *fp);

int vlhe_apply_expand_split(const char *cmd, char *buf, size_t max,
                            char **argv, int argmax);

/*
 * WHO OUR DAEMONS RUN AS - design/33 section 3k, the rulings of
 * 2026-09-30. 1 and `a' filled: an INSTALLED VLHE run by root starts
 * vsoundd, vmidid and vdiscd as the `vlhe' account. 0: run them as we
 * are - a portable copy, which leaves the system as it found it, or
 * anyone not root. -1: installed and root but there is no usable
 * account, which the runner says and then starts them as root rather
 * than not at all. The second form takes the two facts as arguments,
 * for the host tests.
 */
struct vlhe_account;
int vlhe_apply_daemon_account(struct vlhe_account *a);
int vlhe_apply_account_decide(int trial, long euid, struct vlhe_account *a);

/* Is this argv[0] - a path or a bare name - one of our three daemons? */
int vlhe_apply_is_our_daemon(const char *argv0);

/*
 * IS THIS LOAD STEP'S WORK ALREADY DONE? - design/54 D01, 2026-10-03.
 * 1 when the step loads a module that is already resident (insmod,
 * modprobe) or starts one of our daemons that is already running, with
 * a short reason in `why'; the runner then skips it as satisfied rather
 * than running it and failing the press. 0 for everything else - never
 * for an rmmod, a stop, or a pseudo-step. Reads /proc/modules and the
 * pid files (VLHE_PROC_MODULES and VLHE_RUNDIR override, for the tests).
 */
int vlhe_apply_step_satisfied(const struct vlhe_step *st, char *why, int max);

/*
 * AT THE END OF AN UNLOAD: file the session into `sessions/' if every
 * record is UNDONE (vlhe_session_rotate()), else say how many are still
 * in effect - and tell the journal either way, for its verdict.
 * design/54 D25 and D23. The runner calls it; public for the tests.
 */
void vlhe_apply_session_close(FILE *out);

/*
 * ONE LOAD OR UNLOAD AT A TIME - an fcntl() lock on <run dir>/apply.lock,
 * nested (design/54 7h Stage 1). 0 held; 1 another process holds it,
 * `who' its pid; -1 could not be taken (the caller warns and goes on).
 * vlhe_plan_run() takes it; the GUI also holds it across its scoped
 * plans so one press is one run.
 */
/* Point `path' (ProgramsUse) at vsound's node /dev/dsp<idx>, recording
 * the prior state - the Load's (dspnode) step, exposed for the host
 * tests (design/54 D24). 0, or -1. */
int  vlhe_apply_link_dsp(const char *path, int idx, FILE *out);

/*
 * WHAT A LOAD THAT WAS NEVER UNLOADED LEFT BEHIND - design/54 7h Stage
 * 2. Counts the session records still in effect (OPEN, FAILED) whose
 * component's module is NOT loaded - a reboot or power loss clears the
 * modules and leaves the records - and sets `mask' (if given) to the
 * components, bit 1 << VLHE_ENABLE_*. With `out', lists them as `#'
 * lines. 0 means nothing is left over; a Load refuses otherwise.
 */
int  vlhe_apply_leftover(int *mask, FILE *out);

/* How many of the last vlhe_apply_leftover()'s records were FAILED -
 * an unload tried and could not - rather than OPEN, never undone (a
 * shutdown or power loss). The Status page words the two differently. */
int  vlhe_apply_leftover_failed(void);

/* FOR THE HOST TESTS - design/54 D61. Runs `cmd' (no node expansion)
 * with a deadline of `timeout_s' seconds; returns its exit status, -1,
 * or -2 when it did not finish and was stopped. */
int  vlhe_apply_run_for_test(const char *cmd, int timeout_s);

/* OUR DAEMONS' PROGRAM NAMES, NULL-terminated - design/54 D61. */
const char *const *vlhe_apply_daemon_names(void);

/*
 * AT BOOT: finish a load that was never unloaded, before loading -
 * design/54 7h decision 2, under [Boot] FinishLeftover. 0 nothing was
 * left over; 1 finished, the Load may go on; -1 not finished (the key is
 * 0, or something is still FAILED) and the Load must not run. Writes
 * the one-time notice either way.
 */
int  vlhe_apply_boot_finish(FILE *out);

/*
 * AFTER A BOOT'S LOAD: WHICH "Load at startup" DRIVES DID NOT COME
 * BACK - 2026-10-05, found on 86Box with an image on a disk mounted
 * after login. vdiscd -r leaves such a drive empty and says so on
 * output the boot sends to /dev/null; this asks vdiscd what it has
 * attached and writes a one-time notice line for each flagged drive
 * that is empty. Silent when DrivesAutoLoad is 0, when nothing is
 * flagged, or when vdiscd does not answer (CD not in the load).
 * Returns the number of drives named.
 */
int  vlhe_apply_boot_drives(FILE *out);
/* Its comparison, for the host tests: `wanted' is each drive's "comes
 * back" flag AND path (vdiscd_state_wanted()), `reply' vdiscd's
 * `status' answer; fills `missed' with the wanted drives not attached
 * and returns how many. */
int  vlhe_apply_drives_missed(const int *wanted, int ndrives,
                              const char *reply, int *missed, int max);
/* The engine under the boot finish and the Status page's Finish the
 * unload: the scoped unloads of what is left over, in one journal run,
 * under the run lock. Returns the records still left over (0 =
 * finished) or -2 when the lock is held. `why' is noted in the session
 * file; `conflicts' and `still' (may be NULL) get the journal's counts. */
int  vlhe_apply_finish_leftover(const char *why, FILE *out, int *conflicts,
                                int *still);

/*
 * THE ONE-TIME NOTICE beside the session file, for what an unattended
 * boot learned (its output goes to /dev/null). add() appends a
 * timestamped line; take() copies it all to `out' and removes it,
 * returning 1 if there was anything.
 */
int  vlhe_apply_notice_add(const char *text);
int  vlhe_apply_notice_take(FILE *out);

/*
 * FOR THE GUI - design/54 D63. read() copies the notice into `buf'
 * without removing it and records what it saw; clear() removes it only
 * if it is still exactly that, so a line added meanwhile is not lost.
 * read(): bytes read, 0 = no notice, -1 = there but unreadable (try
 * later). clear(): 0 gone, -1 with errno EAGAIN (it changed) or the
 * unlink's errno.
 */
struct vlhe_notice_seen {
    long ino;
    long size;
    long mtime;
};
int  vlhe_apply_notice_read(char *buf, size_t max,
                            struct vlhe_notice_seen *seen);
int  vlhe_apply_notice_clear(const struct vlhe_notice_seen *seen);

int  vlhe_apply_lock(char *who, size_t max);
void vlhe_apply_unlock(void);

#endif /* VLHE_APPLY_H */
