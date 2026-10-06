/*
 * vlhe_self.h - where is this binary, and is it a trial copy?
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHY THIS EXISTS. Trial mode was detected with `stat("./vsound.o")'
 * in FIVE places across three files, and `.' is the PROCESS'S working
 * directory - not the directory the program lives in.
 *
 * THE CASE THAT BREAKS IS THE MAIN ONE. A trial user double-clicks
 * `vlhe.gtk' in a file manager. They have set no environment variable,
 * they have no init script installed, and the working directory is
 * whatever the file manager chose - which may be their home, or `/'.
 * The probe then fails and the program decides it is installed; worse,
 * the config path `./vlhe.conf' would have it offering to create
 * `/vlhe.conf', as root, at the filesystem root. The user saw a
 * version of this on 2026-09-22: "it doesnt know what config to use".
 *
 * /proc/self/exe IS THE FIX. The kernel records what was executed, so
 * the answer holds however the program was launched and from wherever.
 * It is present on the target - `PROC_PID_EXE' at fs/proc/base.c:118
 * in the 2.2.16 tree - and argv[0] is the fallback for a kernel
 * without /proc.
 *
 * THE TWO QUESTIONS ARE SEPARATE, AND CONFLATING THEM WAS THE OTHER
 * HALF OF THE BUG:
 *
 *   am I a trial copy?        vsound.o beside MY BINARY
 *   is VLHE installed here?   /etc/vlhe.conf exists
 *
 * The old code chose trial only when /etc/vlhe.conf was ABSENT, so a
 * user who has VLHE installed and unpacks a newer build to try it -
 * "to see if it addresses any issues they have", the user's own case
 * and the most likely trial of all - got the installed config read
 * into the new binary while its own sat unread beside it. With the
 * questions separated, all four combinations mean something:
 *
 *   trial  installed   what it is
 *   -----  ---------   ----------------------------------------
 *    yes      no       a first trial - use my folder
 *    yes      yes      trying a NEW BUILD - use my folder, and
 *                      offer to copy the installed settings in
 *    no       yes      installed - use /etc
 *    no       no       installed, unconfigured - create /etc
 *
 * A TRIAL COPY IS SELF-CONTAINED. Config, user settings, journal and
 * backups all live in its folder; nothing installed is read or
 * written. Deleting the folder ends the trial.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_SELF_H
#define VLHE_SELF_H

#define VLHE_SELF_MAX 512

/*
 * THE MARKER FILE - A LABEL SINCE 2026-10-04. It once decided the mode;
 * now the build does (vlhe_self_is_trial() below), and the file only
 * tells a person what the folder is. The generators still write it.
 *
 * `PORTABLE' AND NOT `TRIAL-MODE', the user's call 2026-09-22:
 * "trial" reads as a time-limited demo of paid software. This is GPL,
 * nothing is withheld and nothing expires. What it actually describes
 * is a program run from an unpacked folder, keeping its settings
 * beside itself and touching nothing of the system - which is what
 * portable means everywhere else.
 *
 * AN INSTALLED PACKAGE MUST NEVER CONTAIN IT.
 */
#define VLHE_PORTABLE_MARKER "PORTABLE"

/*
 * CALL THIS FIRST, from main(), with argv[0].
 *
 * `argv0' is only used if /proc is unavailable, and may be NULL - the
 * answer is then simply unknown, which every caller handles.
 *
 * Idempotent: the binary cannot move while it runs, so the answer is
 * resolved once and kept.
 */
void vlhe_self_init(const char *argv0);

/*
 * THE DIRECTORY HOLDING THIS BINARY, absolute, with NO trailing
 * slash - except "/" itself, which is all slash.
 *
 * NULL when it could not be determined, which means no /proc and an
 * argv[0] that was a bare name. That combination implies the program
 * was found on PATH, i.e. installed, so every caller reads NULL as
 * "not a trial copy" and carries on.
 */
const char *vlhe_self_dir(void);

/*
 * BUILD AN ABSOLUTE PATH TO `name' BESIDE THIS BINARY.
 *
 * Returns 1 and fills `out', or 0 when the directory is unknown or
 * the result would not fit - in which case `out' is untouched and the
 * caller must fall back rather than use a truncated path, which would
 * name a file that does not exist and fail somewhere less obvious.
 */
int vlhe_self_path(const char *name, char *out, int max);

/*
 * WHERE A PORTABLE BUILD KEEPS ONE KIND OF THING - its fixed layout:
 * "Modules" -> modules/, "Daemons" -> daemons/, "Tools" -> tools/; any
 * other key is beside the binary. Returns 1 and fills `out' with an
 * absolute directory, or 0 when the binary's directory is unknown or
 * the result would not fit.
 *
 * THE INSTALLED BUILD ALWAYS RETURNS 0 - it keeps nothing beside itself.
 *
 * The layout used to be read from keys in the PORTABLE file; it is fixed
 * now (design/55 13f, the user's "known subdirectory structure").
 */
int vlhe_self_subdir(const char *key, char *out, int max);

/*
 * IS THIS THE PORTABLE BUILD? Decided when it was built - `make
 * portable' (VLHE_PORTABLE_BUILD) answers 1, `make' answers 0 - and
 * NOTHING at run time changes it: the PORTABLE file beside the binary
 * is a label for people and neither build reads it (design/55 13f,
 * 2026-10-04). It does not answer "is VLHE installed" - see the table
 * above, which still holds for a portable copy on a machine with VLHE
 * installed.
 */
int vlhe_self_is_trial(void);

/*
 * ROOT DOES NOT TAKE ORDERS FROM THE ENVIRONMENT - design/55
 * recommendation 3, removing R1 and R15 (2026-10-04).
 *
 * A plain `su' (not `su -') keeps the invoking account's environment,
 * and the overrides that make the host tests possible would then steer
 * root: VLHE_MODULE_DIR names the kernel code root insmods, VLHE_SETUP
 * a script root runs, VLHE_SESSION what root unlinks, VLHE_RUNDIR what
 * root chowns, VLHE_USER_CONF the LAME root executes. MODPATH steers
 * insmod's own search the same way, and a user's PATH decides what a
 * bare `insmod', `killall' or fallback daemon name runs.
 *
 * So, in a process whose REAL and effective uid are both 0, this
 * removes every VLHE_* variable, VDISCD_CTL_DIR, VMIDID_CTL_DIR and
 * MODPATH, and sets PATH to root's own directories. Children - the
 * plan's insmod, the daemons, smf2wav - inherit the result.
 *
 * NOT THE SETUID BUILD RUN BY A USER (real uid not 0): vlhe_priv_init()
 * already rebuilds that environment, keeping the user's PATH for the
 * unprivileged work and fixing it only while raised (design/48 S3, S5).
 *
 * THE HOST TESTS ARE UNAFFECTED - they run as a user. A build that must
 * honour the overrides as root (debugging on the target) is compiled
 * with -DVLHE_TRUST_ENV_AS_ROOT; nothing ships that way.
 *
 * Call it first in main(), before anything reads the environment.
 * Idempotent.
 */
void vlhe_self_root_env(void);

/* The scrub itself, done whoever is running - what vlhe_self_root_env()
 * calls for root, and what test_vlhe_self checks as an ordinary user. */
void vlhe_self_env_scrub(void);

/*
 * FILES ROOT WRITES IN A DIRECTORY SOMEONE ELSE CAN WRITE - design/55
 * recommendation 2 (R2, R3), 2026-10-04.
 *
 * The session file, the baseline, the notice and the journal live in
 * the `vlhe' account's directory or a portable folder, and the config
 * writer's .tmp and .old sit beside files in folders users own. fopen()
 * with "w" or "a" FOLLOWS a symlink left at the name, and 2.2.16 has no
 * protection for links in sticky directories (design/55 section 6), so
 * root wrote wherever the link pointed. These open without following:
 *
 *   vlhe_safe_new     a fresh file: the name is unlinked first (unlink
 *                     never follows), then O_CREAT|O_EXCL|O_NOFOLLOW, so
 *                     nothing slipped in between is followed. 0644.
 *   vlhe_safe_append  O_APPEND|O_CREAT|O_NOFOLLOW, and the result must be
 *                     a regular file with one link - a hard link to a
 *                     root file is refused as surely as a symlink.
 *   vlhe_safe_state   READ a file root will act on: a regular file, not
 *                     a link, owned by root or by this process (the host
 *                     tests), never writable by others. A root-owned file
 *                     that is only group-writable - the setuid build's
 *                     umask made those - is tightened to 0644 and read.
 *                     Its directory is NOT judged: in an installed system
 *                     it belongs to the `vlhe' account, which design/55
 *                     recommendation 1 would change (not done); what root
 *                     then does with the contents is held to VLHE's own
 *                     paths by recorded_ok() in vlhe_apply.c.
 *
 * Each returns a stdio stream, or NULL with errno (EPERM for a file
 * refused on its owner, mode or type; ENOENT passes through).
 */
#include <stdio.h>
FILE *vlhe_safe_new(const char *path);
FILE *vlhe_safe_append(const char *path);
FILE *vlhe_safe_state(const char *path);

#endif /* VLHE_SELF_H */
