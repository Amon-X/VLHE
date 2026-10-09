/*
 * vlhe_priv.h - the setuid build's privilege handling.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * ONLY COMPILED INTO vlhe.gtk.su.295, the setuid-root build. The
 * ordinary vlhe.gtk.295 links these too and every one is a no-op, so
 * the GUI has ONE code path and no #ifdef outside this file - which
 * is the thing that keeps the unprivileged build the tested one.
 *
 * WHY SETUID AT ALL, and the decision is the user's, 2026-09-22.
 * design/33 recorded three ways to reach root and declined setuid on
 * the grounds that "a setuid-root GTK program is a far larger
 * commitment than anything this project has made". THAT PREMISE DID
 * NOT HOLD: the user already runs this GUI as root routinely, by
 * `su` and launching it, so the exposure was accepted long ago and
 * setuid changes convenience rather than category.
 *
 * Their reasoning for it over the alternatives:
 *
 *   - MOST OF WHAT THIS PROGRAM DOES NEEDS ROOT. A file manager is
 *     unprivileged nearly always and occasionally is not, so
 *     escalate-on-demand fits it; a control centre is the opposite.
 *   - IT COLLAPSES THE TWO MENU ENTRIES. Corel Explorer ships one
 *     normal and one root entry BECAUSE it is not setuid. One binary
 *     that behaves correctly for both is fewer things to track.
 *   - AND IT IS THE ONLY OPTION THAT WORKS FROM A COLD START.
 *     design/33 names the circularity: delegating to a daemon cannot
 *     load modules for the first time, because no daemon is running
 *     yet. That is exactly trial mode.
 *
 * THE SHAPE IS kcontrol's, read from Corel's own source
 * (`kdelibs/kcontrol.cpp:409`, and they wrote it themselves - every
 * piece carries "BI: 242852 -- 1998/12/08 -- Sam Watts"):
 *
 *     if (getuid() != 0) {
 *         seteuid(getuid());          // drop at once
 *         enableAcceptance(FALSE);    // grey OK and Apply
 *         showSUAButton(TRUE);        // reveal Modify
 *     }
 *
 * and Modify pipes a password to `kcheckpass -s', then `seteuid(0)'.
 *
 * PRIVILEGE IS DROPPED BEFORE gtk_init(), WHICH IS THE POINT. GTK
 * 1.2 reads GTK_MODULES and g_module_open()s whatever it names, and
 * the target's 1.2.7 imports NEITHER getuid NOR geteuid - verified
 * with objdump on usr/lib/libgtk-1.2.so.0.5.2 - so it cannot be
 * checking whether it is setuid and will load them regardless. With
 * the drop first, the toolkit never runs privileged; the environment
 * is scrubbed anyway, because the regain window exists.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_PRIV_H
#define VLHE_PRIV_H

/*
 * CALL THIS FIRST - before gtk_init(), before anything reads the
 * environment. Rebuilds the environment from an ALLOW-LIST (display,
 * locale, PATH; HOME from the password file; GTK_RC_FILES to the
 * system file) so an unprivileged environment cannot steer a program
 * that can become root, then drops effective privilege when the real
 * user is not root. vlhe_priv.c has the list and why each is there.
 *
 * In the ordinary build it scrubs nothing and drops nothing, because
 * there is nothing to drop.
 */
void vlhe_priv_init(void);

/*
 * FOR A CLI SUBCOMMAND - after vlhe_priv_init(), before running it. The
 * setuid build run by a user drops root for good (real, effective and
 * saved uid all the user's, proved by seteuid(0) failing); a subcommand
 * has no Modify, so it never needed the way back. A no-op for root and
 * in the ordinary build. design/54 D49.
 */
void vlhe_priv_drop_for_good(void);

/*
 * IS THERE PRIVILEGE TO REGAIN? True only in the setuid build, run
 * by a non-root user, with the password not yet given. It is what
 * decides whether a Modify button appears at all - root never sees
 * one, exactly as kcontrol's does not.
 */
int  vlhe_priv_can_unlock(void);

/*
 * ASK kcheckpass WHETHER THIS IS THE ROOT PASSWORD, and regain
 * privilege if it is.
 *
 * `password' is not copied and is not retained. The caller should
 * wipe its own buffer afterwards.
 *
 * Returns 0 on success. Otherwise VLHE_UNLOCK_WRONG for a wrong
 * password - the one failure worth retyping for - and -1 for the
 * rest (kcheckpass missing, the database unreadable, privilege not
 * regained), each with a one-line reason in `why'.
 *
 * THE CODE, NOT THE TEXT, IS WHAT A CALLER TESTS - design/47 section
 * 7 fault 3. The GUI decided whether to keep its password dialog
 * open with strstr(why, "not the root password"), so moving that
 * sentence into the strings header, or rewording it, would have
 * silently ended the retry. `why' is for the user to read.
 */
#define VLHE_UNLOCK_WRONG (-2)
int  vlhe_priv_unlock(const char *password, char *why, int max);

/* Has the password been given this session? (In the setuid build
 * that no longer means euid is 0 - see vlhe_priv_raise().) */
int  vlhe_priv_unlocked(void);

/*
 * TAKE ROOT FOR ONE PIECE OF WORK, AND GIVE IT BACK - design/48 S4.
 *
 * After Modify the setuid build holds privilege in its saved uid and
 * uses it only between raise and lower. They nest: only the
 * outermost lower drops. Before Modify, and in the ordinary build,
 * both do nothing.
 *
 * NOT CALLED DIRECTLY BY PAGES. The GUI registers them with
 * vlhe_backend_set_priv() at startup, and the backend raises around
 * the work that needs it - control-channel requests, the system
 * config, reset, the plan runner. A page that wants root for
 * something new should add it there, where the list is kept.
 */
int  vlhe_priv_raise(void);
void vlhe_priv_lower(void);

/*
 * STEP OUT OF EVERY RAISE FOR A WHILE, AND BACK - design/55 R6,
 * design/54 D48 (2026-10-04). For a dialog that must run GTK while a
 * privileged operation is part-way through: the drift question is asked
 * from inside the Load's raised window, and its nested gtk_main() ran
 * every handler and timeout as root for as long as it was open.
 * suspend() lowers completely, whatever the nesting depth, and returns
 * that depth; resume() raises back to it. Both are no-ops when nothing
 * is raised (the ordinary build, or a session that never unlocked).
 * resume() returns -1 if root cannot be regained - the caller's
 * privileged steps then fail and say so, which is the safe direction.
 */
int  vlhe_priv_suspend(void);
int  vlhe_priv_resume(int depth);

/*
 * CAN THIS SESSION DO ROOT'S WORK? The question a page asks before
 * OFFERING a privileged action - greying Load, choosing reset scopes.
 *
 * NOT THE SAME AS vlhe_is_root(), which means "euid 0 NOW" and in the
 * setuid build after Modify is true only inside a raised window.
 * Asking that to decide sensitivity would grey everything the user
 * just unlocked.
 */
int  vlhe_priv_can_act(void);

/*
 * WHERE kcheckpass IS, or NULL when this machine has none. KDE 1.1
 * ships it SETUID ROOT (`-rwsr-xr-x` in kde-corel_1.1-1568.deb), so
 * verification needs nothing of ours - the whole of our side is a
 * pipe and an exit status.
 *
 * A machine without KDE has no way to verify, and the Modify button
 * says so rather than appearing and failing.
 */
const char *vlhe_priv_checkpass_path(void);

/*
 * STRIP THE COMMAND-LINE OPTIONS THAT MAKE GTK LOAD CODE - design/49
 * N1. Call after vlhe_priv_init() and BEFORE gtk_init(). Removes
 * every --gtk-*, --gdk-* and --g-* argument, the path after a bare
 * --gtk-module, and vlhe.gtk's own -F with its font name (N2). A no-op
 * in the ordinary build.
 */
void vlhe_priv_filter_argv(int *argc, char **argv);

#endif /* VLHE_PRIV_H */
