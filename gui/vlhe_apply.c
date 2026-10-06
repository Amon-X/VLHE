/*
 * vlhe_apply.c - the config on disk, turned into commands.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * READ vlhe_apply.h FIRST. It has why this builds a plan rather than
 * running one, and why the machine check is here from the first
 * commit rather than added later.
 *
 * THE EXECUTING HALF ARRIVED 2026-09-21, and this paragraph used to
 * say "NOTHING IN THIS FILE EXECUTES ANYTHING... it does not include
 * <unistd.h> for a reason". That was true while only the planner
 * existed. vlhe_plan_run() is now at the bottom of this file.
 *
 * THE SPLIT IT DESCRIBED STILL HOLDS, and is the point: every
 * decision - which module first, which parameter where, what is
 * optional - is made by plan_load() and plan_unload() and is a pure
 * function of the config that a host test checks line by line.
 * vlhe_plan_run() is a loop with NO decisions in it. Keeping it that
 * way is what makes the untestable part small.
 *
 * WHY THEY SHARE A FILE rather than splitting: the runner needs the
 * step kinds, the `optional' flag and the parenthesis convention
 * that plan_unload() writes, and a header between them would be
 * three declarations describing one idea.
 *
 * C89, GCC 2.95.2.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* the capture's run-<stamp> */
#include <unistd.h>
#include <sys/stat.h>
/* major()/minor()/makedev() - the POSIX spellings, NOT the kernel's
 * MAJOR/MKDEV, which are not visible to userspace. Present in the
 * target's own headers (usr/include/sys/sysmacros.h:28). */
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/ioctl.h>   /* VSOUND_IOC_STAT, below */
#include <dirent.h>      /* the run directory handed to the account */
#include <grp.h>         /* setgroups() - the account's groups */

#include "vlhe_backend.h"
#include "vlhe_status.h"
#include "vlhe_session.h"
#include "vlhe_baseline.h"
#include "vlhe_modconf.h"  /* look-only opens, CLAUDE.md section 5 */
#include "vlhe_journal.h"
#include "vlhe_apply.h"
#include "vlhe_mixer.h"
#include "vlhe_self.h"
#include "vlhe_conf.h"   /* the daemons' account, the drive state */
#include "vdiscd_ctl.h"  /* /dev/cdrom's inner link */
#include "vdiscd_state.h" /* which drives should come back at boot */
#include "vsound.h"      /* VSOUND_IOC_STAT - which node is ours */

/* ------------------------------------------------------------------ */
/* Is this the right machine?                                         */
/* ------------------------------------------------------------------ */

int
vlhe_apply_machine_ok(char *why, size_t len)
{
    struct utsname u;

    if (why != NULL && len > 0)
        why[0] = '\0';

    if (uname(&u) != 0) {
        if (why != NULL)
            strncpy(why, "cannot read the kernel version", len - 1);
        return -1;
    }

    /*
     * SIGNAL 1: the kernel must be 2.2.16, with or without -usb.
     *
     * A PREFIX TEST, deliberately. The target boots 2.2.16 and
     * 2.2.16-usb and the release must work on both (CLAUDE.md
     * section 3: two module packages, one per kernel). Anything else
     * - 2.2.14, 2.4, a modern kernel - is refused.
     */
    if (strncmp(u.release, "2.2.16", 6) != 0) {
        if (why != NULL) {
            /* NO %s AFTER A NUMBER. sh-utils 1.16's printf corrupts
             * that, and while THIS is C's printf and safe, keeping
             * the habit costs nothing - design/07. */
            snprintf(why, len,
                     "this kernel is %s, and vlhe apply is for 2.2.16",
                     u.release);
        }
        return -1;
    }

    /*
     * THERE IS NO SECOND SIGNAL, AND THERE WAS - REMOVED 2026-09-24
     * AT THE USER'S INSTRUCTION, after it refused a load on their
     * own machine.
     *
     * It grepped /etc/modules for the word "kerneld", which on a
     * Corel machine appears ONLY in the eight-line comment header.
     * So editing that file - or letting an editor mangle the header,
     * which is exactly what happened with kwrite - made `vlhe apply'
     * decide this was not a Corel machine and refuse. A COMMENT.
     *
     * AND IT REJECTS A REAL MACHINE: the Deluxe bare-metal one has a
     * ONE-LINE header with no kerneld mention at all (CLAUDE.md
     * section 3 quotes the file).
     *
     * SIGNAL 1 IS SUFFICIENT. `uname -r' beginning 2.2.16 is positive
     * identification of the kernel, which is the thing that actually
     * decides whether our modules can load; no modern machine runs
     * it. CLAUDE.md offers the kerneld grep as a SECOND signal for a
     * session deciding whether it is about to write to the wrong
     * machine's file - a different problem from gating a load, and
     * borrowing it for this was a mistake.
     */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Where the modules are                                              */
/* ------------------------------------------------------------------ */

/*
 * FIND vsound.o AND FRIENDS, and SAY WHICH COPY WAS CHOSEN.
 *
 * TWO CASES, BOTH REAL - design/33 section 3h:
 *
 *   installed      /lib/modules/<uname -r>/misc/   (the .deb's home)
 *   unpacked       the directory the user is in    (trying it out)
 *
 * THE SECOND IS THE FIRST-CONTACT PATH. Someone unpacks a tarball and
 * wants to hear it work before committing to a package; that is how a
 * cautious person evaluates a driver that takes over /dev/dsp, and it
 * is also exactly what every test bundle does.
 *
 * THE PLAN USED TO EMIT A BARE `insmod vsound.o', which works only
 * from the directory holding it - silently right in a test bundle and
 * silently wrong anywhere else.
 *
 * INSTALLED BEATS A STRAY COPY, AND A PORTABLE TREE BEATS INSTALLED.
 * A loose vsound.o in the current directory is likelier to be an old
 * build someone forgot, and is never looked at. A folder carrying the
 * PORTABLE marker has DECLARED itself self-contained, so its modules
 * win over the installed ones (2026-10-02, step 0b below).
 */
static const char *module_dir_find(char *dir, size_t max);

/*
 * REMEMBERED ONLY ONCE FOUND - design/47 A4. Both this and daemon_dir()
 * memoised their FIRST answer for the life of the process, so a GUI
 * started before the files were staged - the normal order on 86Box,
 * where the control centre is up while mkxfer.sh runs - never found
 * them until restarted. An empty answer is asked again next time; a
 * found directory is kept, since files do not move once staged.
 */
static const char *
module_dir(void)
{
    static char dir[256];
    static int  found;

    if (found)
        return dir;
    module_dir_find(dir, sizeof dir);
    found = (dir[0] != '\0');
    return dir;
}

static const char *
module_dir_find(char *dir, size_t max)
{
    struct utsname u;
    struct stat    st;
    char           cand[256];

    dir[0] = '\0';

    /*
     * 0. AN EXPLICIT DIRECTORY, AND IT IS FOR TRIAL MODE.
     *
     * `VLHE_MODULE_DIR' names where our modules are, overriding both
     * searches below. Two callers want it:
     *
     *   - TRIAL MODE. The user's requirement: try the modules
     *     WITHOUT installing anything. Someone extracts the project
     *     and the modules are in that directory, wherever it is.
     *   - THE HOST TESTS, which can then check the emitted commands
     *     against a scratch directory - the same reason VLHE_CONF,
     *     VLHE_RUNDIR and VLHE_PROC_MODULES exist.
     *
     * IT DOES NOT MAKE ANYTHING SAFE TO RUN HERE. Redirecting a path
     * is not the same as not loading a module, and CLAUDE.md section
     * 1 is explicit that this program's load path never runs on the
     * workstation. This override changes what the plan SAYS.
     *
     * CHECKED FOR vsound.o RATHER THAN TAKEN ON TRUST - a typo'd
     * directory should fall through to the real searches and be
     * reported as not found, not silently emit insmod lines pointing
     * at nothing.
     */
    {
        const char *env = getenv("VLHE_MODULE_DIR");

        if (env != NULL && env[0] != '\0'
            && strlen(env) < max - 16) {
            sprintf(cand, "%.200s/vsound.o", env);
            if (stat(cand, &st) == 0) {
                strcpy(dir, env);
                return dir;
            }
        }
    }

    /* 0b. THE PORTABLE TREE, wherever the marker says its modules
     *     are - AHEAD OF THE CONFIG AND THE INSTALLED TREE.
     *
     *     IT WAS STEP 3, AFTER /lib/modules, AND THAT MADE A PORTABLE
     *     TEST ON AN INSTALLED MACHINE LOAD THE INSTALLED MODULES -
     *     silently, from a folder that declares itself self-contained.
     *     Moved 2026-10-02 at the user's instruction, which completes
     *     the reversal vlhe_conf.c records for the config on
     *     2026-09-22: "A PORTABLE TREE IS SELF-CONTAINED ... nothing
     *     installed is read or written". The config followed the
     *     marker from that day; the modules and daemons did not.
     *
     *     AHEAD OF [Paths] ModuleDir TOO. A portable tree's config may
     *     be copied from the installed one (the first-run dialog's
     *     "Copy old settings"), and its ModuleDir then names
     *     /lib/modules - the same leak by another route.
     *
     *     VLHE_MODULE_DIR above still beats it: an explicit directory
     *     named for this run is the one override nothing outranks.
     *
     *     IT USED TO BE `.' AND THAT BROKE AT BOOT. On 86Box
     *     2026-09-22 the init script got as far as `insmod ./vsound.o'
     *     and failed, because an init script's working directory is
     *     `/'. THE PATH IS ABSOLUTE and comes from beside the BINARY,
     *     so it holds however the program was started. `Modules=' in
     *     the marker moves it, so base/modules/ works without touching
     *     this code.
     *
     *     AND IT IS THE ONLY PLACE A PORTABLE BUILD LOOKS - design/55
     *     13f, 2026-10-04. It fell through to ModuleDir and /lib/modules
     *     when its folder had no vsound.o, i.e. loaded the INSTALLED
     *     modules from a portable copy. Now its modules/ is the answer
     *     either way, so a missing module is an insmod naming the folder,
     *     not a quiet switch to someone else's files. (The marker's
     *     `Modules=' key is gone too: the layout is fixed.) */
    if (vlhe_self_is_trial()) {
        if (vlhe_self_subdir("Modules", cand, sizeof cand)
            && strlen(cand) < max)
            strcpy(dir, cand);
        return dir;
    }

    /*
     * 1. `[Paths] ModuleDir' IS NOT READ ANY MORE - 2026-10-05.
     *    It named where an installed VLHE's modules were and beat the
     *    search below. Since the installed load became `modprobe NAME'
     *    (insmod_cmd()), modprobe finds the running kernel's copy
     *    itself and the key could only mislead: on a machine booting
     *    stock 2.2.16 and 2.2.16-usb, a ModuleDir naming one kernel's
     *    directory made every boot of the other report that kernel's
     *    modules as found. What is answered here is now only "is
     *    there a vsound.o for THIS kernel", for the plan's wording. A
     *    config that still carries the key is unaffected - nothing
     *    reads it. (vlhe_module_dir_conf() remains for the backend.)
     */

    /* 2. the installed tree for the RUNNING kernel. Not a guess at
     *    2.2.16 - a machine may boot either, and the modules differ
     *    (MODVERSIONS on -usb, off on stock: CLAUDE.md section 3). */
    if (uname(&u) == 0) {
        sprintf(cand, "/lib/modules/%.60s/misc", u.release);
        strcat(cand, "/vsound.o");
        if (stat(cand, &st) == 0) {
            sprintf(dir, "/lib/modules/%.60s/misc", u.release);
            return dir;
        }
    }

    /* NEITHER. The plan still names the files so a dry run shows what
     * it wanted, with the step marked so the reason is visible. */
    return dir;             /* empty */
}

/*
 * WHERE THE DAEMONS ARE - the same question module_dir() answers for
 * the modules, and it needs answering for the same reason.
 *
 * MEASURED ON TARGET 2026-09-21. The first real `vlhe apply' reached
 * step 6 and stopped:
 *
 *     run   vsoundd -d /dev/dsp1
 *           FAILED rc=127 - stopping
 *
 * 127 is "command not found". The plan named the daemons BARE, which
 * is right for an installed machine where they are in /usr/sbin and
 * on PATH - and wrong for TRIAL MODE, where everything sits in an
 * extracted directory that is not on anyone's PATH. The modules were
 * already absolute by then, because module_dir() had been taught
 * this lesson; the daemons had not.
 *
 * THE ORDER MATCHES module_dir()'s: an explicit override, then a
 * portable tree's declared directory, then the installed location,
 * then beside us. VLHE_BIN_DIR is the override
 * and exists for the same two callers - trial mode, and a host test
 * that wants to see the emitted command without a target.
 *
 * A BARE NAME IS THE LAST RESORT rather than an error, because on a
 * properly installed machine PATH is the right answer and inventing
 * a path would be worse.
 */
/* WHERE `make install' PUT THE DAEMONS - the build's $(sbindir), passed
 * in by the Makefile; this default only for a compile that does not. */
#ifndef VLHE_SBINDIR
#define VLHE_SBINDIR "/usr/local/sbin"
#endif

static const char *daemon_dir_find(char *dir, size_t max);

/* As module_dir(): remembered only once found - design/47 A4. */
static const char *
daemon_dir(void)
{
    static char dir[256];
    static int  found;

    if (found)
        return dir;
    daemon_dir_find(dir, sizeof dir);
    found = (dir[0] != '\0');
    return dir;
}

static const char *
daemon_dir_find(char *dir, size_t max)
{
    struct stat st;
    char        cand[256];
    const char *env;

    dir[0] = '\0';

    env = getenv("VLHE_BIN_DIR");
    if (env != NULL && env[0] != '\0' && strlen(env) < max - 16) {
        sprintf(cand, "%.200s/vsoundd", env);
        if (stat(cand, &st) == 0) {
            strcpy(dir, env);
            return dir;
        }
    }

    /*
     * THE PORTABLE TREE'S `Daemons=' DIRECTORY, AHEAD OF /usr/sbin -
     * 2026-10-02, with module_dir_find()'s step 0b and for the same
     * reason: a portable test on a machine with VLHE installed started
     * the INSTALLED daemons. An absent key means beside the binary, so
     * a flat tree is found here as well.
     *
     * THE KEY IS NEW. The user, the same day: "I think it is better to
     * have an organized folder structure rather than a bunch of files
     * in a single folder" - so the generators put the daemons in
     * daemons/ and the marker says so.
     */
    /*
     * THE PORTABLE BUILD: ITS OWN daemons/, AND NOTHING ELSE - design/55
     * 13f, 2026-10-04. It fell through to /usr/sbin, i.e. a portable
     * copy could start the INSTALLED daemons. The folder's directory is
     * the answer whether or not the file is there, so a missing daemon is
     * a failed start naming the folder.
     */
    if (vlhe_self_is_trial()) {
        if (vlhe_self_subdir("Daemons", cand, sizeof cand)
            && strlen(cand) < max - 16)
            strcpy(dir, cand);
        return dir;
    }

    /*
     * THE INSTALLED BUILD: WHERE `make install' PUT THEM, AND NOTHING
     * ELSE. VLHE_SBINDIR is the build's own $(sbindir) - /usr/local/sbin
     * by default, /usr/sbin in the package - so the daemons are found
     * where this build installed them. /usr/sbin stays as a second
     * answer for a build whose prefix differs from the package that
     * installed the daemons.
     *
     * IT USED TO FALL BACK "BESIDE THE MODULES" AND "BESIDE THIS BINARY",
     * and the second was how a /usr/local install found its daemons at
     * all (only /usr/sbin was checked by name). Both are gone: an
     * installed build run from some folder must not start that folder's
     * daemons as root - the hybrid design/55 13f removes.
     */
    sprintf(cand, "%.200s/vsoundd", VLHE_SBINDIR);
    if (strlen(VLHE_SBINDIR) < max - 16 && stat(cand, &st) == 0) {
        strcpy(dir, VLHE_SBINDIR);
        return dir;
    }
    if (stat("/usr/sbin/vsoundd", &st) == 0) {
        strcpy(dir, "/usr/sbin");
        return dir;
    }
    return dir;                 /* empty - a bare name, found on PATH */
}

/*
 * `<dir>/name', or a bare `name' when nothing was found.
 *
 * THE DIRECTORY IS CARRIED WHOLE - 2026-10-02. Both of these cut it at
 * `%.120s', SILENTLY: a portable tree under a 125-character path got
 * `.../module/vsound.o' for `.../modules/vsound.o' in a host test,
 * and the plan would have insmod'ed a file that does not exist. The
 * directory buffers are 256 (module_dir(), daemon_dir()), so the
 * width is now theirs, and the step's buffer grew to fit
 * (VLHE_CMD_MAX, vlhe_apply.h). A name that still does not fit is
 * refused with a message rather than shortened.
 */
static void
daemon_cmd(char *out, size_t max, const char *name)
{
    const char *d = daemon_dir();

    if (d[0] != '\0' && strlen(d) + 1 + strlen(name) < max)
        sprintf(out, "%s/%s", d, name);
    else {
        if (d[0] != '\0')
            fprintf(stderr, "vlhe: %.60s... is too long a directory"
                            " for %s - using the bare name\n", d, name);
        sprintf(out, "%.20s", name);
    }
}

/*
 * OUR MODULES' LOAD LINE - `modprobe NAME' INSTALLED, `insmod <dir>/
 * NAME.o' PORTABLE. 2026-10-05, the user's call.
 *
 * INSTALLED: modprobe finds the running kernel's copy through its own
 * modules.dep, so a machine with stock 2.2.16 AND 2.2.16-usb - each
 * package's modules in its own /lib/modules/<kernel>/misc - loads the
 * right one; and it loads what each module needs first (soundcore.o
 * for vsound, sound.o, soundlow.o and soundcore.o for vmidi), so the
 * plan no longer names those. It never searches the working
 * directory - modutils 2.1.121's locate_mod_obj() (depmod/
 * conf_file.c:778) takes a `/' as a path and otherwise searches only
 * its path[] sets, all under /lib/modules - so design/55 R15's
 * planted-module risk, below, does not arise for it.
 *
 * PORTABLE: modprobe cannot - the modules are outside /lib/modules,
 * and `modprobe /path/vmidi.o' finds the file and then fails with "no
 * dependency information" (measured on the target, CLAUDE.md section
 * 3). So insmod of the folder's own file, as before, with soundcore
 * and sound.o modprobed first by their own steps.
 *
 * `out' is VLHE_CMD_MAX: a 255-byte directory, a 20-byte name and 300
 * bytes of arguments fit with room to spare. `module' is the file name
 * ("vsound.o"); the installed form drops the ".o".
 *
 * WHEN NOTHING WAS FOUND IT NAMES THE INSTALLED HOME ANYWAY - design/55
 * R15, 2026-10-04. It wrote a bare `insmod vsound.o', and modutils
 * 2.1.121 opens a name containing `.' RELATIVE TO THE WORKING DIRECTORY
 * (insmod.c, "Locate the file to be loaded"); a bare `vsound' is no
 * better, because its default search path begins with `.' and MODPATH
 * replaces it. So root, standing in a directory someone else controls,
 * would load that directory's vsound.o. An absolute path cannot be
 * redirected: the step fails naming where the module should have been,
 * which is the message a missing module deserves. A portable build
 * never gets here - module_dir() answers its modules/ folder either
 * way. */
static void
insmod_cmd(char *out, const char *module, const char *args)
{
    const char    *d = module_dir();
    struct utsname u;

    if (!vlhe_self_is_trial()) {
        char  name[24];
        char *dot;

        sprintf(name, "%.20s", module);
        dot = strstr(name, ".o");
        if (dot != NULL && dot[2] == '\0')
            *dot = '\0';
        sprintf(out, "modprobe %s%s%.300s", name, args[0] ? " " : "",
                args);
        return;
    }

    if (d[0] != '\0')
        sprintf(out, "insmod %.255s/%.20s%s%.300s", d, module,
                args[0] ? " " : "", args);
    else if (uname(&u) == 0)
        sprintf(out, "insmod /lib/modules/%.60s/misc/%.20s%s%.300s",
                u.release, module, args[0] ? " " : "", args);
    else
        sprintf(out, "insmod /lib/modules/misc/%.20s%s%.300s", module,
                args[0] ? " " : "", args);
}

/*
 * A MODULE STEP'S WORDS WHEN OUR MODULES ARE NOT THERE - for the
 * running kernel, installed; in the folder, portable. modprobe's own
 * failure is "can't locate module vsound", which does not say that a
 * machine booting another 2.2.16 needs that kernel's module package -
 * the dual-boot case, 2026-10-05.
 */
static const char *
missing_label(const char *what)
{
    static char buf[200];
    struct utsname u;

    if (vlhe_self_is_trial())
        sprintf(buf, "%.40s - NOT FOUND in this folder's modules/", what);
    else if (uname(&u) == 0)
        sprintf(buf, "%.40s - NOT FOUND for kernel %.40s: install"
                     " vlhe-%.40s-mod, or build from source", what,
                u.release, u.release);
    else
        sprintf(buf, "%.40s - NOT FOUND in /lib/modules", what);
    return buf;
}

/*
 * WHERE THE MODULES ARE, AND THEREFORE WHETHER THIS IS A TRIAL RUN.
 * vlhe_apply.h has the reasoning; this is the same search module_dir()
 * does, reported rather than consumed.
 *
 * DELIBERATELY NOT SHARING module_dir()'s CACHE. That function
 * answers "where do I write insmod lines from" and memoises; this one
 * answers "what is this machine carrying", is called once at startup,
 * and must see the truth rather than an answer cached before the user
 * installed anything.
 */
int
vlhe_where_are_modules(char *dir, int max, int *also_folder)
{
    struct utsname u;
    struct stat    st;
    char  cand[256];
    char  found[256];
    int   where = VLHE_WHERE_NONE;
    int   folder_too = 0;

    found[0] = '\0';

    /* IS THERE A PORTABLE TREE? Asked FIRST because it is also the
     * `also_folder' answer, whichever way the decision goes.
     *
     * THE MARKER AND THE DECLARED DIRECTORY, not `./vsound.o' - the
     * probe this replaced asked about the WORKING directory, so an
     * init script or a double-click got the wrong answer. */
    if (vlhe_self_is_trial()
        && vlhe_self_subdir("Modules", cand, sizeof cand)) {
        char probe[512];

        if (strlen(cand) + sizeof "/vsound.o" < sizeof probe) {
            sprintf(probe, "%s/vsound.o", cand);
            if (stat(probe, &st) == 0) {
                folder_too = 1;
                strncpy(found, cand, sizeof found - 1);
                found[sizeof found - 1] = '\0';
            }
        }
    }

    {
        const char *env = getenv("VLHE_MODULE_DIR");

        if (env != NULL && env[0] != '\0' && strlen(env) < sizeof cand - 16) {
            sprintf(cand, "%.200s/vsound.o", env);
            if (stat(cand, &st) == 0) {
                strncpy(found, env, sizeof found - 1);
                found[sizeof found - 1] = '\0';
                where = VLHE_WHERE_OVERRIDE;
            }
        }
    }

    /* THE PORTABLE TREE BEFORE THE INSTALLED ONE - 2026-10-02, the
     * same order module_dir_find() uses, so this report and the insmod
     * lines cannot disagree. `found' already holds the portable
     * directory from the check at the top - absolute, so nothing
     * downstream depends on where the process happens to be standing. */
    if (where == VLHE_WHERE_NONE && folder_too)
        where = VLHE_WHERE_FOLDER;

    /* NOT FOR A PORTABLE BUILD, which never uses the installed tree. */
    if (where == VLHE_WHERE_NONE && !vlhe_self_is_trial()
        && uname(&u) == 0) {
        sprintf(cand, "/lib/modules/%.60s/misc/vsound.o", u.release);
        if (stat(cand, &st) == 0) {
            sprintf(found, "/lib/modules/%.60s/misc", u.release);
            where = VLHE_WHERE_INSTALLED;
        }
    }

    if (dir != NULL && max > 0) {
        strncpy(dir, found, max - 1);
        dir[max - 1] = '\0';
    }
    if (also_folder != NULL) {
        /* Only interesting when something ELSE won - a folder that IS
         * the answer is not "also". */
        *also_folder = (where != VLHE_WHERE_FOLDER) && folder_too;
    }
    return where;
}

int
vlhe_is_trial(void)
{
    /* THE BUILD DECIDES - design/55 13f. This asked where the modules
     * were, which is a layout, not a mode. */
    return vlhe_self_is_trial();
}

/* ------------------------------------------------------------------ */
/* Building the plan                                                  */
/* ------------------------------------------------------------------ */

/*
 * THE SCOPE THIS PLAN IS BEING BUILT FOR - see vlhe_apply.h.
 *
 * A FILE-SCOPED VARIABLE RATHER THAN A PARAMETER THREADED THROUGH,
 * and that is a deliberate trade. plan_load() and plan_unload() would
 * otherwise take it and pass it to nothing: the nine places that ask
 * are all inside them, and every one already calls
 * vlhe_component_enabled(). Wrapping THAT is a one-word change at
 * each site; threading a parameter is a change to every signature
 * between here and there.
 *
 * IT IS SET AND CLEARED BY vlhe_plan_build_scoped() around the build,
 * which is the only writer. Nothing here is re-entrant or threaded -
 * a plan is built in one call, on one thread, on a 2.2 uniprocessor
 * kernel - so the usual objection to this shape does not apply.
 */
static int g_scope = VLHE_SCOPE_ALL;

/*
 * THE PRESS ITSELF IS THE CONSENT - design/43, added 2026-09-26 at
 * the user's ask.
 *
 * When set, the ONE component named by `g_scope' is planned whether
 * or not its Include-in-load box is ticked. Everything else is
 * unaffected: this is not "ignore the checkboxes", it is "this
 * particular button names its own component".
 *
 * WHY IT IS WANTED. The tickboxes are a standing choice about what a
 * full Load includes and what happens at boot. The per-row Load
 * buttons exist because the user was CLICKING THOSE BOXES to load
 * components one at a time - *"I was hitting the check boxes to load
 * each component separately what is why i asked for load buttons"* -
 * which writes /etc/vlhe.conf on every experiment and leaves the
 * machine's boot setting wherever the session happened to stop.
 *
 * SO A BUTTON THAT THEN REFUSED BECAUSE THE BOX IS CLEAR WOULD SEND
 * THEM BACK TO THE BOX. "Nothing to load - check what is enabled" is
 * an honest message and the wrong answer to a control whose whole
 * purpose is to avoid touching that setting.
 *
 * IT DOES NOT CHANGE THE SETTING. The plan runs; the config is not
 * written; the next full Load still honours the box. That is the
 * difference between doing a thing once and choosing it.
 *
 * THE UNLOAD SIDE NEVER SETS IT, and does not need to: in_scope()
 * already ignores `enabled' for teardown, for the reason the comment
 * below gives.
 */
static int g_scope_forced;

/*
 * IS THIS COMPONENT IN THIS PLAN? Enabled AND in scope.
 *
 * THE TWO ARE DIFFERENT QUESTIONS and both must pass. `enabled' is
 * the user's standing choice, in the config; `scope' is what THIS
 * plan is for. A component the user has turned off stays out of a
 * scoped plan that names it - pressing Reload on a disabled component
 * must not quietly enable it.
 */
static int in_scope(int which);
static int  node_record_first(const char *path, const char *det,
                              FILE *out);
static void node_record_unmade(const char *path);
static void baseline_seed(const char *path, const char *vsnode, FILE *out);
static int  dsp_target_dangling(const char *target);
static void note_made(const char *note, char *out, size_t max);
static const char *how_fix(void);

/*
 * WHICH SET OF SWITCHES A PLAN OBEYS - 2026-10-02, the user: "Status
 * shouldn't touch boot behaviour." The control centre's Load reads
 * the Include boxes (vlhe_component_enabled()); the command line's
 * `vlhe apply' - which is what the init script runs at boot - reads
 * LoadAtBoot (vlhe_component_at_boot()). They were one key before.
 * The CLI sets this once at start; the GUI never does.
 */
static int g_plan_at_boot;

void
vlhe_plan_from_boot_keys(int on)
{
    g_plan_at_boot = on ? 1 : 0;
}

static int
in_plan(int which)
{
    if (!in_scope(which))
        return 0;
    /* THE FORCED COMPONENT IS IN, AND ONLY IT. `g_scope_forced' is
     * meaningless without a single-component scope, so the equality
     * test is what limits it - a forced VLHE_SCOPE_ALL could not
     * match and would enable nothing extra. */
    if (g_scope_forced && which == g_scope)
        return 1;
    return g_plan_at_boot ? vlhe_component_at_boot(which)
                          : vlhe_component_enabled(which);
}

/*
 * IS THIS COMPONENT IN SCOPE, WHETHER OR NOT IT IS ENABLED?
 *
 * THE UNLOAD SIDE USES THIS AND THE LOAD SIDE MUST NOT - the user,
 * 2026-09-26, asking for the Enable boxes: "It only affects loading.
 * It shouldnt affect unloading".
 *
 * WHY THE DISTINCTION IS NOT PEDANTRY. `enabled' is a choice about
 * what to BRING UP. Applied to teardown it says "do not remove the
 * thing you were told not to load", which strands exactly the case a
 * user creates while troubleshooting: load with MIDI on, untick MIDI
 * to try the next combination, unload - and vmidi stays in the
 * kernel, because in_plan() returned false and no `rmmod vmidi' was
 * ever planned. The module-loaded guard beneath does not save it:
 * that one SKIPS a step for a module that is absent, it cannot add
 * one for a module that is present.
 *
 * SCOPE STILL APPLIES, which is the half plan_unload() was right
 * about: a CD-scoped unload must not stop the synth. The original
 * comment said in_plan() was shared "so the two directions cannot
 * disagree about what a scope means", and that reasoning holds for
 * scope and silently carried `enabled' along with it.
 *
 * SO TEARDOWN ASKS WHAT IS LOADED, NOT WHAT IS WANTED. Every removal
 * below is already guarded by vlhe_status_module_loaded(), which is
 * the correct question for an unload.
 */
static int
in_scope(int which)
{
    return g_scope == VLHE_SCOPE_ALL || g_scope == which;
}

/* Append a step. Silently marks the plan truncated rather than
 * overflowing - a plan that lost a step must not look complete. */
/* WHICH COMPONENT THE STEPS BEING ADDED BELONG TO - set by the
 * builders in front of each component's block, 0 for a plan-wide
 * step, and stamped on every step so the simulation can head each
 * group "#---- vsound ----" (the user, 2026-10-01). */
static int g_comp;

static void
add(struct vlhe_plan *p, int kind, int optional,
    const char *why, const char *cmd)
{
    struct vlhe_step *s;

    if (p->n >= VLHE_PLAN_MAX) {
        p->truncated = 1;
        return;
    }

    s = &p->step[p->n++];
    s->kind     = kind;
    s->optional = optional;
    s->comp     = g_comp;
    /* EXPLICITLY ZEROED, not left to the caller's struct being
     * fresh. A plan built twice into the same storage would
     * otherwise inherit the previous run's cleanup flags, and a
     * spurious cleanup step is one that runs after a failure when
     * nothing needs undoing. */
    s->cleanup  = 0;

    strncpy(s->cmd, cmd, sizeof s->cmd - 1);
    s->cmd[sizeof s->cmd - 1] = '\0';
    strncpy(s->why, why, sizeof s->why - 1);
    s->why[sizeof s->why - 1] = '\0';
}

/*
 * MARK THE STEP JUST ADDED AS CLEANUP - see vlhe_apply.h.
 *
 * A SEPARATE CALL RATHER THAN A SIXTH ARGUMENT TO add(), because
 * exactly one step in two plans wants it and widening the signature
 * would put a `0' on twenty-odd call sites to say "not that one".
 *
 * SILENT WHEN THE PLAN IS FULL. add() has already set `truncated' in
 * that case and returned without appending, so there is no step to
 * mark; marking p->step[p->n - 1] regardless would flag the WRONG
 * step - the last one that did fit.
 */
static void
mark_cleanup(struct vlhe_plan *p)
{
    if (p != NULL && p->n > 0 && !p->truncated)
        p->step[p->n - 1].cleanup = 1;
}

/*
 * THE LOAD ORDER, AND IT IS NOT ARBITRARY.
 *
 *   1. sound.o        vmidi imports midi_synth_* and
 *                     sound_alloc_mididev from it; without it insmod
 *                     fails with unresolved symbols (design/09:2150).
 *                     modprobe rather than insmod, so its own
 *                     dependencies come too - AND WITH THE `.o',
 *                     which bypasses sndconfig's `alias sound
 *                     <card>'. See the comment at the call.
 *   2. vsound.o       BEFORE the real card's driver, or applications
 *                     bind to the card and silently bypass the mixer
 *                     (CLAUDE.md section 5).
 *   3. vmidi.o        needs sound.o above it.
 *   4. vdisc.o        independent of the sound stack; last because
 *                     nothing else waits on it.
 *
 * Then the daemons, each after its module.
 */
/*
 * WHERE A DAEMON SHOULD PLAY, AS A PLAN STEP SAYS IT - design/43
 * part A, section 3, AND REVISED 2026-09-26 AFTER THE FIRST TARGET
 * RUN.
 *
 * IT EMITS ONE OF TWO THINGS:
 *
 *   SOUND in the plan     -> "@VSOUND@"
 *   SOUND not in the plan -> "@VSOUND:@CARD@@"
 *
 * THE SECOND IS THE CORRECTION. It first emitted a bare "@CARD@",
 * which was right for the moment the plan was built and WRONG for
 * the rest of the daemon's life: `@CARD@' expands here, at run
 * time, into a literal path, and vlhe_resolve_out() passes a
 * literal through unchanged forever. The run of 2026-09-26 18:15
 * showed it exactly - MIDI and CD loaded first with Sound unticked,
 * so both got `/dev/dsp1'; vsound came up four minutes later and
 * NEITHER MOVED, `vsoundd: stopping, 0 bytes written`.
 *
 * `@VSOUND:<path>@' CARRIES BOTH ANSWERS and lets the daemon choose
 * at every open: vsound when it is loaded, that card when it is
 * not. So a user who never loads vsound still gets sound, and one
 * who loads it later is picked up without a restart - which is what
 * the user asked for and what neither single token could do.
 *
 * NOTE THE NESTING IS DELIBERATE AND SAFE. `@CARD@' is still
 * expanded for these daemons (only `@VSOUND@' is held back), so the
 * plan text stays readable while the daemon receives a real path
 * inside the tag.
 *
 * WHY NOT ALWAYS THE COMPOSITE. With SOUND in the plan there is no
 * second choice to offer: vsound is being loaded by this very plan,
 * so "not loaded yet" means WAIT, and falling back to a card would
 * be the silent mode-change design/36 row 53 measured as "the odd
 * note here or there of music".
 */
/*
 * AND IT ASKS `enabled', NOT `in_plan' - CORRECTED 2026-09-28 AFTER
 * A TARGET RUN WHERE EVERYTHING WAS TICKED AND THE SYNTH STILL DIED.
 *
 * THE BUG: `in_plan()' is SCOPE-AWARE, and the Load button builds
 * THREE SEPARATE PLANS - Sound, then MIDI, then CD, one per scope
 * (`vlhe_mod_status.c:703'). While the MIDI plan is being built
 * `g_scope == VLHE_ENABLE_MIDI', so `in_plan(VLHE_ENABLE_SOUND)' is
 * FALSE even though the Sound plan loaded vsound a moment earlier.
 * vmidid was therefore handed `@VSOUND:@CARD@@' - the "there is no
 * vsound, use your card" form - on a machine that was loading
 * vsound in the same click.
 *
 * WHAT THE USER SAW: *"load had everything ticked ... vmidid got a
 * pid but then shut down"*. The daemon starts, resolves the token,
 * finds `/proc/vsound' not there YET because `vsoundd' came up
 * seconds earlier in a different plan, falls back to the CARD -
 * which `vsoundd' already holds - and its first write returns EIO.
 * Six of eight cycles in
 * `tests/logs/2026-09-28-esssolo1-and-es1371-cdg' show it; the one
 * clean run simply won the race.
 *
 * AND RESTART COULD NOT HELP, which is what made it look like a
 * daemon fault: the per-row Restart builds a MIDI-scoped plan too,
 * so it emitted the same wrong token every time.
 *
 * THE RIGHT QUESTION IS ABOUT THE MACHINE, NOT THIS PLAN. "Will
 * vsound be there when this daemon runs?" is answered by the user's
 * Include-in-load setting, which `vlhe_component_enabled()' reads
 * from the config and which no scope can change. A scoped plan that
 * loads only MIDI on a machine whose Sound box is ticked still
 * wants `@VSOUND@', because vsound either is loaded or is being
 * loaded beside it.
 *
 * IT WAS NOT THE ESS SOLO-1, though that is where it was found. The
 * card only decides WHICH failure you get: `esssolo1' opens without
 * O_NONBLOCK and yields EIO where another driver would have
 * returned EBUSY and printed "all vsound channels are in use".
 *
 * AND THE FORCED CASE IS KEPT, which a bare `enabled' test would
 * have broken the other way: the Sound row's own Load button loads
 * vsound with the box UNTICKED (`vlhe_plan_build_forced'), and
 * there `enabled' is 0 while the plan plainly does load it. So the
 * test is EITHER - forced into this plan, or enabled on this
 * machine - and only "neither" earns the fallback.
 */
static const char *
out_token(void)
{
    int sound = vlhe_component_enabled(VLHE_ENABLE_SOUND)
             || (g_scope_forced && g_scope == VLHE_ENABLE_SOUND);

    return sound ? "@VSOUND@" : "@VSOUND:@CARD@@";
}

static void
plan_load(struct vlhe_plan *p)
{
    struct vlhe_sound    snd;
    struct vlhe_synth    sy;
    struct vlhe_midiopts mo;
    struct vlhe_modopts  cd;
    struct vlhe_font     fonts[VLHE_MAX_FONTS];
    char cmd[VLHE_CMD_MAX];
    int  nf, i;

    /* ZEROED SO NO FIELD IS EVER READ UNINITIALISED. The original
     * reason was narrower and is gone with the displacement design:
     * a reload step read `snd.card_module' outside the block that
     * filled it, guarded only by a `contested' flag. Neither exists
     * now - there is no reload and no contest - but the memset is
     * kept, because `vlhe_sound()' can fail and leave every field
     * untouched. */
    memset(&snd, 0, sizeof snd);

    /*
     * `sound.o' WITH THE SUFFIX, AND THAT IS THE WHOLE POINT.
     *
     * THE USER, 2026-09-19, and it was their reason for writing it
     * that way: a BARE `sound' hits an alias. sndconfig writes
     *
     *     alias sound es1371          (/etc/conf.modules)
     *
     * on a machine whose card it detected, and modutils resolves an
     * alias BEFORE looking for a module of that name - so `modprobe
     * sound' loads THE SOUND CARD, not the OSS core. Measured on
     * tests/vm/fresh.img after running sndconfig by hand.
     *
     * `modprobe sound.o' names the file and bypasses the alias.
     * CLAUDE.md section 3 already records that modutils 2.1.121
     * accepts both spellings, proven by a machine that boots with
     * six bare and four suffixed lines; what the suffix BUYS us is
     * this - it is unambiguous where the bare name is not.
     *
     * AND vmidi IS THE REASON WE NEED IT AT ALL. It imports
     * midi_synth_* and sound_alloc_mididev from sound.o
     * (design/09:2150). vsound does NOT - it wants
     * register_sound_dsp, which soundcore.o exports and sound.o
     * merely imports as well (verified with nm on the fresh
     * install's own modules). So the two halves of this project
     * depend on DIFFERENT modules, and an earlier version of this
     * comment had vsound needing sound.o, which is wrong.
     */
    /*
     * soundcore FIRST, AND UNCONDITIONALLY - vsound AND vmidi both
     * need it.
     *
     *   vsound.o  register_sound_dsp, unregister_sound_dsp
     *   vmidi.o   register_sound_special, unregister_sound_special
     *
     * Verified with `nm' on the fresh install's own modules, not
     * assumed. modules.dep says `misc/soundcore.o:' with nothing
     * after the colon, so it depends on nothing and is safe to name
     * first.
     *
     * OPTIONAL BECAUSE IT IS USUALLY ALREADY THERE - any sound card
     * driver holds it, so on most machines this is a no-op. It is
     * named anyway because TRIAL MODE cannot assume a card driver
     * was ever loaded, and `insmod' of vsound.o against an absent
     * soundcore is an unresolved-symbol failure rather than a
     * helpful message.
     */
    /* PORTABLE ONLY SINCE 2026-10-05 - an installed load modprobes our
     * modules, and modprobe loads soundcore with them (insmod_cmd()). */
    g_comp = VLHE_ENABLE_SOUND;
    if (vlhe_self_is_trial())
        add(p, VLHE_STEP_MODULE, 1,
            "exports register_sound_dsp - vsound and vmidi both need it",
            "modprobe soundcore");

    /*
     * MOVE A LEGACY CARD OFF DEVICE 0 - the step that was missing
     * until 2026-09-21, and whose absence produced a failure nobody
     * could read.
     *
     * THE SYMPTOM. On a fresh boot of 86Box, `sb' loads from
     * /etc/modules and takes device 0. `vlhe apply' then inserted
     * vsound with no contest, vsound got minor 19, and /dev/dsp
     * stayed pointed at the real SB16. vsoundd worked - it plays OUT
     * to /dev/dsp1 - so everything looked healthy until vmidid's
     * O_EXCL open of /dev/dsp hit the actual card and was refused
     * -EBUSY. The daemon reported "the reserved MIDI slot is already
     * held - is another vmidid running?", which is the one cause it
     * knows and was the wrong one.
     *
     * WHY IT APPEARED INTERMITTENT, which is the part worth
     * recording: running vsound/tests/load.sh and then unload.sh
     * MADE IT WORK, and a reboot broke it again. load.sh rmmods the
     * card, loads vsound at device 0, then modprobes the card back -
     * onto device 1. unload.sh removes vsound and LEAVES THE CARD
     * THERE. So device 0 stays vacant for the rest of that boot and
     * a later `vlhe apply' succeeds by inheriting a vacancy it never
     * created. The CLI had never once contested device 0; it only
     * sometimes ran on a machine where something else had.
     *
     * CLAUDE.md section 5 states the requirement plainly: "vcdsnd
     * must load before the user's sound card driver, or applications
     * bind to the real card and silently bypass the mixer."
     *
     * AN UNNAMED HOLDER IS REFUSED, NOT UNLOADED. load.sh:394 has
     * the precedent - "Unloading whatever happens to be there is a
     * different and much less safe thing than unloading the one
     * driver this script knows about" - and vlhe_backend.h's
     * card_module comment has why the name must come from the user:
     * a self-built module is one we cannot know.
     *
     * AND THE REFUSAL HALTS THE RUN. Continuing past it means
     * insmod'ing vsound into a slot where it never receives
     * /dev/dsp, so the plan would fail three steps later at vmidid
     * with a message about a busy MIDI slot - which is precisely the
     * misdirection this whole block exists to end.
     */
    /*
     * EACH COMPONENT IS GATED ON ITS OWN SETTING, added 2026-09-22.
     *
     * The three `LoadAtBoot' keys existed in the template and NOTHING
     * READ THEM - so turning MIDI off changed the file and not the
     * plan, and every apply loaded all three regardless. The user
     * asked how to pick one, two or all three from the GUI; the
     * answer is that the setting was already there and this is what
     * makes it mean something.
     *
     * THE NAME IS WRONG AND IS KEPT ANYWAY. "At boot" is meaningless
     * to a portable copy - no init script is installed, so nothing of
     * ours loads at boot there - which is why the GUI says `Enable'.
     * Renaming the KEY would strand every config already written.
     */
    /*
     * THE CARD IS NOT DISPLACED ANY MORE - design/38 section 9,
     * 2026-09-25, and this is where the contest used to be.
     *
     * It rmmod'd the user's card so vsound could take minor 3 and
     * therefore /dev/dsp, then modprobe'd it back. That worked and
     * it cost too much: a load-order rule, arithmetic about which
     * node each would land on, the card's module arguments needed
     * or the reload fails - and twice on 2026-09-24 a `modprobe
     * sb' that failed for want of them left a machine WITH NO
     * CARD AT ALL.
     *
     * vsound now loads wherever there is room and `/dev/dsp'
     * becomes a SYMLINK to it, the way `/dev/cdrom' already
     * works. The card keeps its own node the whole time, and
     * name_the_card() gives it one first if `/dev/dsp' was its
     * only name.
     *
     * WHAT DID NOT CHANGE: vmidi still lands behind the card in
     * the MIDI table, because the card was already being reloaded
     * BEFORE vmidi in this plan. The phantom at index 0 is the
     * card's raw MIDI port either way (design/38 7b) and removing
     * the contest neither causes nor cures it.
     */

    /*
     * vsound TAKES NO RATE OR MINOR AS A MODULE PARAMETER in the
     * config - the mix rate belongs to the pump. What it does take
     * is the MIDI slot (vsound_midi) and, since 2026-10-02, the
     * limiter and attenuation (vsound_limit, vsound_atten).
     */
    g_comp = VLHE_ENABLE_SOUND;
    if (in_plan(VLHE_ENABLE_SOUND)) {
        /*
         * TRACING ONLY WHEN THE CONFIG ASKS - `[Tracing] Enabled',
         * vlhe_tracing(). design/09's decision (the user, 2026-09-01):
         * off by default, switchable at runtime, one setting. Built
         * 2026-10-02, the day the session-only checkboxes that did
         * this for the 86Box esssolo1 EIO hunt were removed.
         *
         * OFF MEANS NOTHING ON THE LINE: vsound_trace is an
         * uninitialised global in vsound_dev.c. ON STATES ALL THREE
         * RATES, because vsound_rate_mix DEFAULTS TO 1 in the module
         * and `vsound_trace=1' alone floods the log - four chan[]
         * lines per tick per slot, which once made a 12 MB capture
         * that was 96% that one message. Opens, releases and triggers
         * (life) are cheap and are what makes a trace readable.
         */
        char args[160];
        int  an = 0;

        args[0] = '\0';
        if (vlhe_sound(&snd) == 0) {
            if (snd.midi_slot >= 0)
                an = sprintf(args, "vsound_midi=%d", snd.midi_slot);
            /* THE MIX'S HEADROOM - design/09, built 2026-10-02. Only
             * when not the module's own default (limiter 1, 100%), so
             * an ordinary line stays as short as it was. */
            if (snd.limiter != 1)
                an += sprintf(args + an, "%svsound_limit=%d",
                              an ? " " : "", snd.limiter);
            if (snd.attenuation != 100)
                an += sprintf(args + an, "%svsound_atten=%d",
                              an ? " " : "", snd.attenuation);
        }
        if (vlhe_tracing())
            sprintf(args + an, "%svsound_trace=1 vsound_rate_mix=0"
                    " vsound_rate_write=0 vsound_rate_life=1",
                    an ? " " : "");
        insmod_cmd(cmd, "vsound.o", args);
        add(p, VLHE_STEP_MODULE, 0,
            module_dir()[0] ? "the mixer" : missing_label("the mixer"),
            cmd);

        /*
         * AND THE NODE FOR WHATEVER MINOR IT GOT - design/38 9f2.
         *
         * IMMEDIATELY AFTER THE insmod AND BEFORE ANYTHING OPENS IT.
         * On a stock machine MAKEDEV has made two dsp nodes and a
         * two-DSP card can hold both, so vsound lands on a minor
         * with no node - loaded and unreachable. The number is read
         * from /proc/vsound at run time, which is why this is a
         * pseudo-step rather than a command with the number in it.
         *
         * OPTIONAL: a machine where the node already exists needs
         * nothing done, and one where mknod fails should still get
         * its report rather than a plan that stops halfway.
         */
        /*
         * THE DESCRIPTION NAMES BOTH HALVES - corrected 2026-09-26,
         * design/36 row 86. It read "/dev/dspN for vsound - made if
         * the stock nodes are taken", which is the node half only;
         * the step ALSO points the user's device at that node, and
         * that is the half a user cares about. A plan line that
         * omits what a step does is a plan line nobody can check
         * against the result - and the redirect went missing for
         * three loads without the journal looking any different.
         */
        add(p, VLHE_STEP_NODE, 0,
            "vsound's own /dev/dspN, and point ProgramsUse at it",
            "(dspnode)");

        /*
         * EACH PROGRAM'S LEVEL, BACK INTO THE MODULE - Save Program
         * Levels (design/33 section 1c, 2026-10-02). Straight after
         * the node, BEFORE vmidid, vdiscd or anything else can open a
         * channel: the module applies a remembered level at open, so
         * the table has to be there first. Optional, like the mixers.
         */
        if (vlhe_progvol_enabled())
            add(p, VLHE_STEP_NODE, 1,
                "each program's volume, as the last unload saved it",
                "(progrestore)");
    }


    g_comp = VLHE_ENABLE_MIDI;
    if (in_plan(VLHE_ENABLE_MIDI)
        && vlhe_midiopts(&mo) == 0) {
        char args[64];

        /*
         * `sound.o' HERE RATHER THAN AT THE TOP, AND ONLY WHEN vmidi
         * IS IN THE PLAN - changed 2026-09-21.
         *
         * It used to be the first step of EVERY plan, which loaded 90
         * KB of OSS core on a run that wanted only vdisc. vmidi is
         * the sole reason we need it: it imports eighteen symbols
         * from sound.o (midi_synth_*, sound_alloc_mididev), where
         * vsound imports NONE and wants soundcore alone.
         *
         * THE USER ASKED FOR THE PARTS TO BE TRIABLE SEPARATELY, and
         * the plan could not express that while this was
         * unconditional.
         *
         * IT STILL PRECEDES vsound.o IN THE FILE ORDER? NO - and that
         * is fine. The ordering rule that matters is `vsound before
         * the CARD'S driver' (CLAUDE.md section 5), so that
         * applications bind to the mixer rather than the card.
         * sound.o is the OSS core, not a card driver, and takes no
         * DSP slot - so vsound having gone in first costs nothing.
         * What must hold is sound.o BEFORE vmidi.o, which it does,
         * three lines down.
         */
        /* PORTABLE ONLY SINCE 2026-10-05: `modprobe vmidi' loads sound.o
         * (and soundlow.o, soundcore.o) from modules.dep, by FILE name,
         * so sndconfig's `alias sound <card>' cannot intercept it. */
        if (vlhe_self_is_trial())
            add(p, VLHE_STEP_MODULE, 1,
                "vmidi imports midi_synth_* from it - .o bypasses any alias",
                "modprobe sound.o");

        /*
         * `vmidi_minor', NOT `minor' - the module's own name for it
         * (vmidi_mod.c:161). MEASURED ON TARGET 2026-09-21, where
         * the first real `vlhe apply' printed
         *
         *     /mnt/xfer/vmidi.o: invalid parameter minor
         *
         * and the step was optional, so the run carried on with the
         * module NOT loaded - which is the quiet version of this
         * failure and the reason it is worth a comment.
         *
         * The vdisc line three lines down already got this right
         * (`vdisc_major', `vdisc_ndevs') and its comment says why:
         * insmod silently ignores an unknown parameter on some
         * builds and refuses on others. This one was simply missed.
         */
        /* LEVEL 2 ADDS THE PER-BYTE LINES (vmidi_rate_byte, off by
         * default in the module); level 1 is the events alone. */
        sprintf(args, "vmidi_minor=%d%s", mo.minor,
                vlhe_tracing() >= 2 ? " vmidi_trace=1 vmidi_rate_byte=1"
                : vlhe_tracing() ? " vmidi_trace=1" : "");
        insmod_cmd(cmd, "vmidi.o", args);
        add(p, VLHE_STEP_MODULE, 0,
            module_dir()[0] ? "the MIDI device"
                            : missing_label("the MIDI device"), cmd);

        /*
         * AND ITS NODE, IMMEDIATELY AFTER - see make_vmidi_node().
         *
         * AFTER the insmod and not before, for the same reason the
         * vdisc nodes are: the module must have claimed the minor
         * before a node naming it means anything.
         *
         * THE NUMBER COMES FROM THE CONFIG, the same `mo.minor' the
         * insmod line above was built from, so the two cannot
         * disagree - which is the whole point of doing it here rather
         * than leaving it to the user's mknod.
         *
         * OPTIONAL, like the vdisc node step: a wrong node is bad and
         * a plan that stopped here would leave vmidi loaded with the
         * sound half of the machine unconfigured. It reports.
         */
        sprintf(cmd, "(vmidinode minor %d)", mo.minor);
        add(p, VLHE_STEP_NODE, 0,
            "/dev/vmidi - recreated if the minor moved", cmd);
    }

    g_comp = VLHE_ENABLE_CD;
    if (in_plan(VLHE_ENABLE_CD)
        && vlhe_modopts(&cd) == 0) {
        char args[112];
        /* THE REAL PARAMETER NAMES, which are vdisc_-prefixed
         * (vdisc_mod.c:167ff) - `major=' would be silently ignored by
         * insmod and the module would take its default. */
        /* vdisc_trace IS THE LEVEL ITSELF - 2 adds the data-read
         * packets (vdisc_mod.c). */
        sprintf(args, "vdisc_major=%d vdisc_ndevs=%d vdisc_packet=%d%s",
                cd.major, cd.ndevs, cd.packet ? 1 : 0,
                vlhe_tracing() >= 2 ? " vdisc_trace=2"
                : vlhe_tracing() ? " vdisc_trace=1" : "");
        insmod_cmd(cmd, "vdisc.o", args);
        add(p, VLHE_STEP_MODULE, 0,
            module_dir()[0] ? "the virtual CD-ROM"
                            : missing_label("the virtual CD-ROM"), cmd);

        /*
         * AND THE NODES, IMMEDIATELY AFTER - see make_nodes() for
         * what this fixes and why it is not a shell command. The
         * numbers are in the step text so a dry run shows what will
         * be made without making it.
         *
         * NOT OPTIONAL SINCE 2026-10-02 - and nor are the insmod
         * above it, the daemon below, vmidi's module, node and
         * daemon, or vsound's /dev/dsp redirect. They were all
         * optional so that "a plan whose sound half may still be
         * wanted" ran on past a failure - the reason vlhe_apply.h
         * records for the scoped plans. Each component is its own
         * plan now, so that is given by structure, and `optional'
         * had come to mean a component could not fail: the user
         * filled the transfer volume (design/36 row 121), the nodes
         * were refused, vdiscd was started with no node and died,
         * and the box said "Done." with three components not
         * running. A failed step now stops ITS component, runs its
         * cleanup steps, and counts; the others still run. What
         * stays optional is what is nice to have: modprobe of a
         * core that is usually already there (its absence fails the
         * insmod after it, which is the honest place), the
         * /dev/cdrom link, and the two restores, which are cleanup.
         */
        sprintf(cmd, "(nodes vdisc major %d, %d drive%s)",
                cd.major, cd.ndevs, cd.ndevs == 1 ? "" : "s");
        add(p, VLHE_STEP_NODE, 0,
            "/dev/vdiscN and /dev/vdiscctl - remade if the major moved",
            cmd);

        /*
         * AND `/dev/cdrom', IF ASKED. Only when the option is on -
         * it points a path we do not own at a drive of ours, and a
         * user who did not ask for that must not get it.
         *
         * AFTER the nodes, because the link's target has to exist
         * before pointing anything at it.
         *
         * THE RESTORE IS A CLEANUP STEP further down, so a plan that
         * fails after this point still puts the user's real drive
         * back - the same invariant as the sound card.
         */
        if (cd.link_cdrom) {
            add(p, VLHE_STEP_NODE, 1,
                "/dev/cdrom -> /dev/vdisc0, saving what it was",
                "(cdromlink)");
        }
    }

    /*
     * THE PUMP. -d names the real card, and it is the one flag that
     * MUST be right: with no -d it opens /dev/dsp, which is vsound
     * itself once the module is in, and a pump feeding its own mixer
     * is silence.
     *
     * AND IT IS THE USER'S PICK AGAIN - design/38 section 9, and
     * this comment described the design BEFORE that section.
     *
     * IT SAID the pick was "the node vsound takes" and the card's
     * node afterwards was "a consequence of the shuffle". That was
     * true while applying a config UNLOADED the card and let it come
     * back wherever the allocator put it. The symlink design removed
     * the shuffle: nothing is unloaded, the card keeps the minor it
     * booted with, and `CardDevice' names it both before and after.
     *
     * SO probe_card() PREFERS THE PICK and probes only when there is
     * none. What survives from the old note is the hazard it was
     * written about: on 2026-09-24 a plan carried `-d /dev/dsp' and
     * DAEMON.LOG read `pumping /dev/dsp -> /dev/dsp', the pump
     * reading from and writing to vsound itself. That is why the
     * pick is checked against vsound's own node before it is used.
     */
    /* THE PUMP GOES WITH ITS MODULE - starting vsoundd with no
     * vsound.o loaded would leave it opening a device that is not
     * there. */
    g_comp = VLHE_ENABLE_SOUND;
    if (in_plan(VLHE_ENABLE_SOUND)
        && vlhe_sound(&snd) == 0) {
        char bin[300];
        int  n;

        daemon_cmd(bin, sizeof bin, "vsoundd");
        /*
         * BOTH ENDS OF THE PUMP, AND `-v' WAS THE ONE THIS FORGOT.
         *
         * `-d' is the CARD it writes to, derived above. `-v' is
         * VSOUND it reads from - which is the node the user PICKED,
         * because that is where vsound lands (design/38). vsoundd
         * defaults it to /dev/dsp, so a pick of anything else left
         * the pump reading a node vsound was not on: DAEMON.LOG said
         * `this module mixes at a fixed rate (no MIXRATE ioctl)' -
         * the real card answering, not us - then `0 bytes written',
         * while vmidid and vdiscd wrote correctly to the pick and
         * nothing joined them up. Found on target 2026-09-25 with
         * two cards, picking `sb'; it worked on the es1371 only
         * because that pick IS /dev/dsp.
         */
        /* BOTH ENDS ARE RESOLVED AT RUN TIME - see expand_nodes().
         *
         * @VSOUND@ GENUINELY CANNOT BE KNOWN NOW: the module has not
         * been inserted, so nothing has allocated it a minor.
         *
         * @CARD@ USUALLY CAN, because it is the user's pick and the
         * card does not move - but it is left a placeholder so that
         * the empty-pick case is resolved the same way as any other,
         * in one place, rather than half here and half there. */
        n = sprintf(cmd, "%.290s -d @CARD@ -v @VSOUND@", bin);
        if (snd.release_on_idle)
            n += sprintf(cmd + n, " -R");
        /* THE MIX RATE, ONLY WHEN SET - [Sound Settings] MixRate, design/54
         * D14. The key has said `-> vsoundd -S' since it was written and
         * nothing passed it, so setting MixRate = 48000 for an emu10k1 (the
         * key's own example) did nothing. 0, the default, asks the card. */
        if (vlhe_mix_rate() > 0)
            sprintf(cmd + n, " -S %d", vlhe_mix_rate());
        add(p, VLHE_STEP_DAEMON, 0,
            "the pump: moves mixed audio to the real card", cmd);
    } else if (in_plan(VLHE_ENABLE_SOUND)) {
        /* THE FALLBACK IS FOR A CONFIG WE COULD NOT READ, not for a
         * component the user turned off - without this `else if' a
         * disabled sound stack still planned its pump, which is the
         * one step that would then fail for having no module under
         * it. Found by testing a CD-only plan, 2026-09-22. */
        add(p, VLHE_STEP_DAEMON, 0,
            "the pump: moves mixed audio to the real card", "vsoundd");
    }

    /*
     * THE SYNTH, and -s HAS NO DEFAULT ON PURPOSE. design/07: "a
     * daemon that silently picks up a font from a path nobody named
     * is a daemon whose output cannot be attributed." So a config
     * with no font produces a step that SAYS so rather than a
     * command that would fail obscurely.
     */
    g_comp = VLHE_ENABLE_MIDI;
    nf = in_plan(VLHE_ENABLE_MIDI)
         ? vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL) : 0;
    if (nf > 0) {
        char bin[300];
        int  n;

        daemon_cmd(bin, sizeof bin, "vmidid");
        n = sprintf(cmd, "%.290s", bin);

        /*
         * THE FONTS ARE PLACEHOLDERS, NOT PATHS - design/36 row 91.
         *
         * THIS USED TO WRITE THE PATH ITSELF, and two of them at the
         * `%.150s' cap put a single step within ~120 bytes of
         * `VLHE_CMD_MAX'. That is data in a structure buffer: the
         * step exists to say WHAT will run, and a soundfont path is
         * an argument whose length has nothing to do with the plan.
         *
         * THE PATTERN IS ALREADY HERE TWICE. `@VSOUND@' and `@CARD@'
         * are resolved by `expand_nodes()' at run time for the same
         * reason, and `vdiscd' went further - its image arrives over
         * a control channel and never touches a command line at all
         * (`vdiscd.c:887', after the mandatory argument broke a load
         * on 2026-09-21). Fonts cannot use the FIFO, because vmidid
         * needs the soundfont at STARTUP to allocate voices, so the
         * placeholder is the right half of that pattern.
         *
         * WHAT THE PLAN NOW SHOWS is `-s @FONT0@', which is also
         * better to read than a 150-character path in a printed plan.
         */
        for (i = 0; i < nf; i++) {
            if (fonts[i].path[0] == '\0')
                continue;
            if (i >= VLHE_FONT_TAGS)
                break;          /* more fonts than tags - see the
                                 * header. Two is what vmidid takes. */
            if (fonts[i].bank > 0)
                n += sprintf(cmd + n, " -s @FONT%d@@%d", i, fonts[i].bank);
            else
                n += sprintf(cmd + n, " -s @FONT%d@", i);
        }
        /*
         * EVERY SETTING THE OPTIONS TAB OFFERS - design/36 row 90.
         *
         * THIS PASSED `-p' ALONE AND THE PAGE HAS SIX CONTROLS. The
         * other five saved to the config correctly and reached
         * nothing: `vmidid' does not read the config (no reference to
         * `vlhe_conf' anywhere in it) and there is no ioctl carrying
         * them, so the command line is the only channel and it was
         * carrying one sixth of the page.
         *
         * THE PAGE PROMISED OTHERWISE: "Turning effects off is the
         * escape on a machine without the headroom for them" - and
         * unticking it changed nothing. The template has said
         * `-> vmidid -g' and `-> vmidid -e' since the keys were
         * written, so this was always the intent.
         *
         * THE SPELLINGS ARE THE DAEMON'S, NOT THE ENUM'S. `-L' and
         * `-F' parse NAMES (`sf2_mod_law_parse', `sf2_mod_vf_parse'),
         * and `-E' takes on/off - passing the integer would fail the
         * parse and take the daemon down with a usage error.
         */
        if (vlhe_synth(&sy) == 0) {
            static const char *const vf[] = { "awe", "2.01", "2.04",
                                              "none" };

            /*
             * AND THE LENGTH IS CHECKED, WHICH IT WAS NOT BEFORE.
             * Two fonts at 150 characters each already put the worst
             * case near `VLHE_CMD_MAX' (512 then; 1024 since 2026-10-02); the flags below add
             * ~49 more and would carry it past. The old code was
             * unguarded too - it was simply far enough under.
             *
             * `room' leaves space for the ` -o @VSOUND@' appended
             * after this block, which must never be the thing that
             * gets truncated: the daemon would then play to its own
             * default device rather than vsound.
             */
            int room = (int) sizeof cmd - 24;

            if (n < room)
                n += sprintf(cmd + n, " -p %d", sy.voices);
            /* ON IS THE DAEMON'S DEFAULT, so only off is stated. */
            if (n < room && !sy.auto_voices)
                n += sprintf(cmd + n, " -A off");
            /* AND ITS TUNABLES, only when the file moved one from the
             * defaults ([Midi Settings] AutoVoice*); a set the check
             * refuses is left off, so the daemon runs its defaults. */
            if (sy.auto_voices) {
                int  av[VLHE_AUTOVOICE_N], moved = 0;
                char spec[128];

                if (vlhe_autovoice_settings(av, &moved, NULL) == 0
                    && moved && n + 4 + (int) sizeof spec < room) {
                    vlhe_autovoice_spec(av, spec);
                    n += sprintf(cmd + n, " -a %s", spec);
                }
            }
            if (n < room)
                n += sprintf(cmd + n, " -g %d.%03d",
                             sy.gain_milli / 1000, sy.gain_milli % 1000);
            if (n < room)
                n += sprintf(cmd + n, " -L %s",
                             sy.law == VLHE_LAW_LINEAR ? "linear"
                                                       : "spec");
            if (n < room && sy.filter >= 0 && sy.filter <= VLHE_VF_NONE)
                n += sprintf(cmd + n, " -F %s", vf[sy.filter]);
            if (n < room)
                n += sprintf(cmd + n, " -E %s",
                             VLHE_FX_WORD(sy.reverb, sy.chorus));
            /* FAST IS THE DAEMON'S DEFAULT, so only reference is
             * stated - design/54 D28. */
            if (n < room && sy.modenv == VLHE_MODENV_REFERENCE)
                n += sprintf(cmd + n, " -M reference");

            /* RATE LAST AND ONLY WHEN SET - the key defaults to 44100
             * and the daemon's own default is the same, so passing it
             * always would add noise to every command line for
             * nothing. A machine that needs 22050 is the exception. */
            if (n < room && sy.rate > 0 && sy.rate != 44100)
                n += sprintf(cmd + n, " -r %d", sy.rate);
        }
        /* THE CHANNEL RELEASE - [Midi Settings] ChannelRelease, only
         * when moved from the daemon's own 3000 (2026-10-03, the user:
         * "key with guard" - the guard is vmidid's, which now counts
         * the effects' tail as sound). */
        {
            int rel = vlhe_midi_release_ms();

            if (rel != 3000 && n < (int) sizeof cmd - 40)
                n += sprintf(cmd + n, " -R %d", rel);
        }
        /*
         * AND WHERE TO PLAY - vsound's node, which is the node the
         * user PICKED (design/38). The daemon defaults to /dev/dsp,
         * which is only vsound's when the pick is device 0; on a
         * machine where the pick is /dev/dsp2 that default reaches
         * whatever the shuffle left behind. Found on target
         * 2026-09-24, two cards, pick /dev/dsp2.
         */
        /* VERBOSE WHEN TRACING IS ON - `[Tracing] Enabled', with the
         * modules' trace parameters above. Before `-o', and inside
         * the same 24-byte reserve `room' keeps for it above, because
         * `-o' must stay the last thing appended. */
        if (vlhe_tracing() && n < (int) sizeof cmd - 24)
            n += sprintf(cmd + n, " -v");
        /* WHERE TO PLAY - vsound when this plan loads it, the
         * user's card when it does not. See out_token(). */
        sprintf(cmd + n, " -o %s", out_token());
        add(p, VLHE_STEP_DAEMON, 0, "the synth", cmd);
    } else if (in_plan(VLHE_ENABLE_MIDI)) {
        /*
         * ONLY WHEN MIDI IS ENABLED. A note saying "no font
         * configured" on a plan where the user turned MIDI OFF
         * describes a problem they do not have - same shape as the
         * pump's fallback above, found in the same test.
         *
         * PARENTHESISED, SO THE RUNNER SKIPS IT. This used to read
         * `vmidid -s <no font configured>', which was fine while
         * nothing executed a plan - it printed as an explanation.
         * vlhe_plan_run() would now hand those angle brackets to
         * execvp and get a confusing failure instead of the clear
         * statement the line is for.
         *
         * The parenthesis is the marker step_is_note() looks for.
         */
        add(p, VLHE_STEP_DAEMON, 1,
            "the synth - NO SOUNDFONT CONFIGURED, so it would refuse",
            "(vmidid - no font configured)");
    }

    g_comp = VLHE_ENABLE_CD;
    if (in_plan(VLHE_ENABLE_CD)) {
        char bin[300];

        daemon_cmd(bin, sizeof bin, "vdiscd");
        /*
         * THE DRIVE STATE, AND WHETHER IT COMES BACK - 2026-10-03,
         * design/33 sections 3c and 3i. `-s' always: the daemon keeps
         * the record of every attach, eject and "Load at startup" box
         * whether or not anything is reattached. `-r' only when
         * [CD Settings] DrivesAutoLoad = 1, the master switch the user
         * kept beside the per-drive boxes. The same line at boot, at a
         * portable Load and at a Status-page restart - so all three
         * bring the flagged drives back.
         */
        /* CD audio goes to vsound too - the same reasoning as the
         * synth above, and the same flag. */
        sprintf(cmd, "%.290s -s @DRIVES@%s -o %s", bin,
                vlhe_drives_autoload() ? " -r" : "", out_token());
        add(p, VLHE_STEP_DAEMON, 0, "the disc server", cmd);
    }

    /*
     * THE CARDS' MIXER LEVELS, AS THE LAST UNLOAD SAVED THEM - Save
     * Mixer Levels (2026-10-02; vlhe_mixer.c has the account). LAST in
     * the load, after everything else: on a boot this is S60, long
     * after S20modutils loaded the card from /etc/modules and the
     * driver set its own defaults, which is what the restore has to
     * come after - the user's point. SOUND SCOPE ONLY, so the control
     * centre's per-component Load restores once per press, not three
     * times. Optional: a failed restore is not a failed load.
     */
    g_comp = VLHE_ENABLE_SOUND;
    if (in_plan(VLHE_ENABLE_SOUND) && vlhe_mixer_restore_enabled())
        add(p, VLHE_STEP_NODE, 1,
            "the sound cards' mixer levels, as the last unload saved them",
            "(mixerrestore)");

    /*
     * AND IF ANY OF THAT FAILS, PUT `/dev/cdrom' BACK.
     *
     * A CLEANUP STEP, LAST IN THE PLAN, and it runs ONLY on the
     * failure path - a successful load reaches the end and leaves the
     * link pointed at us, which is what the user asked for.
     *
     * THE SAME INVARIANT AS THE SOUND CARD: if we redirected a path
     * the user's real drive depends on and then could not finish,
     * they must not be left with `/dev/cdrom' naming a virtual drive
     * that never came up. That is worse than not having tried, and it
     * is exactly the state the user described hitting by hand -
     * "I forgot it was set and had to swap it back".
     *
     * ADDED UNCONDITIONALLY rather than under `cd.link_cdrom',
     * because the run that set the link and the run that fails may be
     * different runs with the option changed in between; the saved
     * file decides whether there is anything to undo, and with no
     * file this does nothing.
     */
    g_comp = 0;
    add(p, VLHE_STEP_NODE, 1,
        "/dev/cdrom back to what it was, if the load failed",
        "(cdromrestore)");
    /* MARKED IMMEDIATELY, BECAUSE mark_cleanup() MARKS THE LAST STEP
     * ONLY. Adding a second restore below and then calling it once
     * moved the flag onto that one and took it OFF this - so the
     * /dev/cdrom restore stopped running on the failure path, which
     * is the whole reason it exists. Caught by test_vlhe_apply's
     * "marked CLEANUP" assertion, 2026-09-25. */
    mark_cleanup(p);

    /* AND /dev/dsp, for the same reason and on the same path: a load
     * that moved the link and then failed must not leave programs
     * pointed at a mixer that is not running. design/38 section 9. */
    add(p, VLHE_STEP_NODE, 1,
        "/dev/dsp back to what it was, if the load failed",
        "(dsprestore)");
    mark_cleanup(p);
}

/*
 * THE UNLOAD ORDER IS NOT THE LOAD ORDER REVERSED, QUITE.
 *
 * Daemons must all stop before any module goes, because a daemon
 * holding a device keeps its module busy and `rmmod' fails. Within
 * the modules, vsound is LAST: vmidi writes into it, so removing
 * vsound first leaves vmidi writing to nothing.
 *
 * AND `sound' IS NEVER REMOVED. CLAUDE.md section 3: rmmod opl3
 * leaves num_synths pointing at a NULL slot and breaks playmidi
 * until reboot. The same class of damage applies to the core, it is
 * shared with whatever else the machine runs, and we did not put it
 * there in the sense that matters - modprobe may have found it
 * already loaded.
 */
/*
 * HOW THE DAEMONS ARE STOPPED IS AN OPEN QUESTION, and the plan says
 * so rather than committing to a command that may not exist.
 *
 * TWO PROBLEMS, both found 2026-09-19 while writing this:
 *
 *   1. `killall' IS NOT GUARANTEED. It comes from psmisc, and psmisc
 *      is NOT in Corel's required_base (CLAUDE.md section 4 has the
 *      66-package list). procps is - so `ps' and `kill' exist
 *      everywhere and `killall' does not.
 *   2. THE DAEMONS WRITE NO PIDFILE. Nothing in vsoundd, vmidid or
 *      vdiscd touches /var/run, so there is no pid to signal even if
 *      the tool were there.
 *
 * The right fix is for the daemons to write /var/run/<name>.pid and
 * for this to read it - which is a change to three programs and
 * belongs in its own commit. Until then the unload plan NAMES the
 * daemon and leaves the mechanism blank, so a dry run tells the truth
 * instead of printing a command that would fail on a stock machine.
 */
/*
 * THE SIMULATION'S "AS IF LOADED" - 2026-10-01. plan_unload() removes
 * only the modules that ARE loaded, so a simulated unload built on a
 * clean machine would be empty. While vlhe_plan_simulate() builds its
 * unload half, a component is treated as loaded if the simulated load
 * would have loaded it - "this assumes every command succeeded".
 */
static int g_assume_loaded;

static int
loaded_or_assumed(int which, const char *module)
{
    if (g_assume_loaded)
        return in_plan(which);
    return vlhe_status_module_loaded(module);
}

static void
plan_unload(struct vlhe_plan *p)
{
    /*
     * SAVE THE CARDS' MIXER LEVELS FIRST - Save Mixer Levels. At
     * shutdown this is the K40 stop; the next load's (mixerrestore)
     * puts them back. Sound scope, optional, as the restore.
     */
    g_comp = VLHE_ENABLE_SOUND;
    if (in_scope(VLHE_ENABLE_SOUND) && vlhe_mixer_restore_enabled())
        add(p, VLHE_STEP_NODE, 1,
            "save the sound cards' mixer levels, for the next load",
            "(mixersave)");
    /* AND EACH PROGRAM'S LEVEL, while vsound is still loaded to ask -
     * Save Program Levels; the load's (progrestore) pushes it back. */
    if (in_scope(VLHE_ENABLE_SOUND) && vlhe_progvol_enabled())
        add(p, VLHE_STEP_NODE, 1,
            "save each program's volume, for the next load",
            "(progsave)");

    /*
     * THE DAEMONS ARE STOPPED FIRST, AND THEY HAVE TO BE - built
     * 2026-09-21 after three consecutive `vlhe apply -u' runs on
     * 86Box stopped at the same step:
     *
     *     run   rmmod vsound
     *     rmmod: vsound: Device or resource busy
     *           FAILED rc=1 - stopping
     *
     * A module cannot be removed while a daemon holds a channel in
     * it, so skipping these made the unload side unable to finish
     * unaided - the user had to know three process names and kill
     * them by hand.
     *
     * THEY KEEP THEIR PARENTHESES even though they now DO something.
     * The runner matches on "(stop " and acts; anything else in
     * parentheses is still a note. That keeps a dry run reading as
     * prose, and keeps the string permanently unable to reach
     * execvp, which is what the parenthesis convention was for.
     *
     * stop_daemon() above has the safety reasoning - the short
     * version is SIGTERM only, never on a pid /proc does not confirm
     * is that daemon, and a timeout reports rather than escalating.
     *
     * ALL THREE WRITE PIDFILES, which a note here denied until the
     * Status page showed vmidid's own ("stale pid file - it died").
     * vsoundd.c:236, vdiscd.c:858 and vmidid's are all real.
     */
    /*
     * SCOPED, LIKE THE LOAD SIDE - added 2026-09-23, and the test
     * caught its absence. This function was written as a whole-stack
     * teardown, so scoping only plan_load() was half the job: a
     * CD-scoped unload still stopped the synth and removed vsound,
     * which is precisely what "reload vdisc not the entire stack"
     * exists to avoid.
     *
     * in_scope() AND NOT in_plan(), CORRECTED 2026-09-26. This said
     * "in_plan() RATHER THAN A SECOND TEST, so the two directions
     * cannot disagree about what a scope means" - right about scope,
     * and it carried `enabled' along with it. Teardown must not ask
     * whether a component is WANTED, only whether it is in scope and
     * actually loaded; see in_scope() for what that stranded.
     */
    g_comp = VLHE_ENABLE_CD;
    if (in_scope(VLHE_ENABLE_CD))
        add(p, VLHE_STEP_DAEMON, 1, "stop the disc server",
            "(stop vdiscd)");
    g_comp = VLHE_ENABLE_MIDI;
    if (in_scope(VLHE_ENABLE_MIDI))
        add(p, VLHE_STEP_DAEMON, 1, "stop the synth", "(stop vmidid)");
    g_comp = VLHE_ENABLE_SOUND;
    if (in_scope(VLHE_ENABLE_SOUND))
        add(p, VLHE_STEP_DAEMON, 1, "stop the pump - vsound cannot be "
            "removed while it holds a channel", "(stop vsoundd)");

    /*
     * ONLY THE MODULES THAT ARE ACTUALLY LOADED - added 2026-09-22,
     * after `/etc/init.d/vlhe stop' reported "some parts remain" on a
     * machine where NOTHING of ours remained.
     *
     * WHAT HAPPENED. The start had refused at the device-0 contest -
     * correctly, the config predated CardModule - so the only thing
     * it had done was `modprobe soundcore', which is a no-op on a
     * machine where sb already loaded it. Nothing of ours was in the
     * kernel. The stop then planned `rmmod vsound' regardless, and
     * `rmmod vsound' is THE ONE NON-OPTIONAL STEP in this plan, so
     * its failure took the exit status non-zero and the init script
     * printed its "some parts remain" line.
     *
     * The user's lsmod and ps proved otherwise - no vsound, no vmidi,
     * no vdisc, no daemons - and pointed out the message named no
     * part and pointed at a journal that held nothing about the stop.
     *
     * REMOVING A MODULE THAT IS NOT THERE IS NOT A FAILURE. The
     * desired state has already been reached, so the step does not
     * belong in the plan at all - which is better than making it
     * optional, because a plan printed by `-n' should describe what
     * will happen rather than list work that will be skipped.
     *
     * AND THE CARD STEP BELOW ALREADY DID THIS. It has guarded on
     * vlhe_status_module_loaded() since it was written, for the same
     * reason in the other direction: not modprobing a card onto a
     * machine that never had one. The three removals simply never
     * got the same treatment.
     *
     * THE ORDER IS UNCHANGED where the modules ARE loaded: vdisc,
     * vmidi, then vsound last because vmidi writes into it.
     */
    g_comp = VLHE_ENABLE_CD;
    if (in_scope(VLHE_ENABLE_CD) && loaded_or_assumed(VLHE_ENABLE_CD, "vdisc"))
        add(p, VLHE_STEP_MODULE, 1, "the virtual CD-ROM", "rmmod vdisc");
    g_comp = VLHE_ENABLE_MIDI;
    if (in_scope(VLHE_ENABLE_MIDI) && loaded_or_assumed(VLHE_ENABLE_MIDI, "vmidi"))
        add(p, VLHE_STEP_MODULE, 1, "the MIDI device",    "rmmod vmidi");
    /*
     * AND vsound STAYS LAST WITHIN WHATEVER IS IN SCOPE, because
     * vmidi writes into it. A MIDI-scoped unload removes vmidi and
     * leaves vsound, which is the right order for that scope too -
     * there is nothing above vsound left to hold it.
     */
    g_comp = VLHE_ENABLE_SOUND;
    if (in_scope(VLHE_ENABLE_SOUND) && loaded_or_assumed(VLHE_ENABLE_SOUND, "vsound"))
        add(p, VLHE_STEP_MODULE, 0, "the mixer, last - vmidi writes into it",
            "rmmod vsound");

    /*
     * SAID ONCE, WITH SOUND - 86Box 2026-10-03. Every scoped plan added
     * it, and since it files under Sound's heading, the CD and MIDI
     * scopes of one press each printed a stray "#---- vsound ----" block
     * saying it - three times per Unload.
     */
    g_comp = 0;
    if (in_scope(VLHE_ENABLE_SOUND))
        add(p, VLHE_STEP_CHECK, 1,
            "`sound' is LEFT LOADED on purpose - see design/09",
            "(sound.o is not removed)");

    /*
     * AND THE NODES, AFTER THE MODULES ARE OUT - see remove_nodes().
     *
     * AFTER and not before, because a node is how a client reaches
     * the module: removing it first would take away the name while
     * the driver is still loaded and possibly still open, which
     * changes what an `rmmod' failure means and helps nobody.
     *
     * THE JOURNAL'S VERDICT DEPENDED ON THIS. vlhe_journal_end()
     * prints "the machine is as it was before the apply" on an undo
     * run, and until this step existed that sentence was FALSE - up
     * to ten device nodes survived every teardown. The claim is now
     * one the teardown actually delivers.
     *
     * OPTIONAL: a node that will not unlink is worth reporting and is
     * not worth halting a teardown over. vlhe_journal_failed() marks
     * it NOT UNDONE, which is precisely the line the journal header
     * calls the most important thing the file can carry.
     */
    /*
     * `/dev/cdrom' FIRST, BEFORE THE NODES GO.
     *
     * The restore points the link back at the user's real drive, and
     * doing it before /dev/vdisc0 is removed means the link is never
     * left naming a node that has just ceased to exist - even for the
     * few milliseconds between the two steps.
     *
     * NOT GATED ON THE OPTION, unlike the load side's. The option may
     * have been turned OFF since the apply that set the link, and a
     * teardown that then skipped the restore would strand it pointing
     * at a drive that is about to go. The saved file is what decides
     * whether there is anything to do - no file, nothing happens.
     *
     * BUT GATED ON THE SCOPE - 2026-09-30, and this and the /dev/dsp
     * step below were the ONLY steps in this function that were not.
     * A Restart of vmidid builds a MIDI-scoped unload, and this step
     * ran in it: it pointed /dev/cdrom back at the real drive while
     * vdisc and vdiscd stayed up. The dsp step below did the same to
     * /dev/dsp, which is how one vmidid Restart broke every program
     * started AFTER it - the link to vsound was gone, the card's own
     * node was behind the name, and the pump holds that exclusively.
     * A program already running kept its open fd and never noticed.
     * tests/logs/2026-09-30-86box-vmidid-restarts-traced/vlhe-changes,
     * 21:07:29: "remove node /dev/dsp back to node 14 3" inside an
     * undo whose module step was vmidi's. The session record for the
     * link survives a scoped unload (remove_recorded_nodes() skips
     * /dev/dsp by name), so the sound-scoped unload that eventually
     * runs still finds what to restore.
     */
    g_comp = VLHE_ENABLE_CD;
    if (in_scope(VLHE_ENABLE_CD))
        add(p, VLHE_STEP_NODE, 1,
            "/dev/cdrom back to what it was, if we changed it",
            "(cdromrestore)");

    /* AND /dev/dsp. Asks first if something else has moved it since -
     * design/38 9g, the user's decision. Sound scope only - above. */
    g_comp = VLHE_ENABLE_SOUND;
    if (in_scope(VLHE_ENABLE_SOUND))
        add(p, VLHE_STEP_NODE, 1,
            "/dev/dsp back to what it was, if we changed it",
            "(dsprestore)");

    /*
     * ONE STEP PER COMPONENT, NOT ONE FOR ALL THE NODES - corrected
     * 2026-09-23 after a target run printed "nodes removing ours"
     * three times, once per scope, each doing the whole job.
     *
     * IT WAS NOT ONLY NOISE. remove_nodes() took EVERY node of ours,
     * so a CD-scoped unload removed /dev/vmidi as well - a node
     * belonging to a component that was not being touched, while its
     * module stayed loaded. The node would then be absent until
     * something reloaded vmidi.
     */
    g_comp = VLHE_ENABLE_CD;
    if (in_scope(VLHE_ENABLE_CD))
        add(p, VLHE_STEP_NODE, 1,
            "/dev/vdiscN and /dev/vdiscctl - only ours", "(rmnodes cd)");
    g_comp = VLHE_ENABLE_MIDI;
    if (in_scope(VLHE_ENABLE_MIDI))
        add(p, VLHE_STEP_NODE, 1,
            "/dev/vmidi - only ours", "(rmnodes midi)");

    /*
     * PUT THE CARD BACK ON DEVICE 0 - and this is the step whose
     * ABSENCE from vsound/tests/unload.sh made the load-side bug
     * look intermittent.
     *
     * unload.sh leaves the card wherever load.sh put it, which is
     * device 1. So after one load/unload cycle device 0 stays vacant
     * for the rest of the boot, and the NEXT `vlhe apply' succeeds
     * by inheriting that vacancy - while a fresh boot, with the card
     * back on device 0 from /etc/modules, fails. Same command, two
     * outcomes, and the difference is invisible unless you know the
     * slot moved. That cost an evening on 2026-09-21.
     *
     * A CYCLE SHOULD LEAVE THE MACHINE AS IT FOUND IT. Reloading the
     * card here restores the boot arrangement, so `apply' then
     * `apply -u' is a round trip rather than a one-way change to the
     * device numbering.
     *
     * ONLY WHEN THE CARD IS ACTUALLY LOADED, checked rather than
     * assumed: a machine that never had a legacy card, or whose user
     * removed it themselves, must not have one modprobe'd in by a
     * teardown. And only when the config names one - we do not guess
     * (vlhe_backend.h's card_module has why).
     */
    {
        /*
         * AND NO CARD CYCLE HERE EITHER - design/38 section 9,
         * 2026-09-25. The unload used to rmmod the card and modprobe
         * it back, so that it returned to device 0 now that vsound
         * had let go. It is the other half of the contest the load
         * side no longer runs: nothing displaced the card, so nothing
         * has to put it back.
         *
         * What DOES have to be undone is the symlink, and
         * "(dsprestore)" below does that.
         */
    }
}

/*
 * RECORD A STEP THAT SUCCEEDED, in the change journal's terms.
 *
 * THE VERB DECIDES THE KIND, NOT st->kind, and that is the point. A
 * VLHE_STEP_MODULE step can go either way: the device-0 contest
 * rmmods the card on the way IN, and filing that as a load would make
 * the apply and its undo fail to match - which is the one thing the
 * journal has to get right.
 *
 * WHAT GOES IN `detail' is what an auditor would need to reverse the
 * change by hand, which for a module is its arguments. Everything
 * after the module's name qualifies; the path prefix does not, so
 * `insmod /mnt/xfer/vsound.o vsound_midi=1' records the module as
 * `vsound.o' with detail `vsound_midi=1'.
 */
/*
 * THE MODULE A STEP LOADS, stripped of its directory, or "" if the
 * step does not load one.
 *
 * SO A FAILURE CAN SAY WHY. `insmod' returns 1 for a module that is
 * already in, and the plan reported that as "(optional, rc=1 -
 * continuing)" - three of them stacked up on a second Load and read
 * as three things going wrong, on a machine that was entirely
 * healthy. Found by the user 2026-09-23, pressing Load twice.
 *
 * `vsound.o' -> `vsound', which is what /proc/modules calls it.
 */
static void
step_module(const struct vlhe_step *st, char *out, int max)
{
    const char *p = st->cmd;
    const char *arg;
    char       *slash, *dot;
    int         n;

    out[0] = '\0';
    if (strncmp(p, "insmod ", 7) == 0)
        p += 7;
    else if (strncmp(p, "modprobe ", 9) == 0)
        p += 9;
    else
        return;

    while (*p == ' ')
        p++;
    arg = strchr(p, ' ');
    n = arg != NULL ? (int)(arg - p) : (int)strlen(p);
    if (n > max - 1)
        n = max - 1;
    memcpy(out, p, n);
    out[n] = '\0';

    slash = strrchr(out, '/');
    if (slash != NULL)
        memmove(out, slash + 1, strlen(slash + 1) + 1);
    /* `.o' IS THE FILE, NOT THE MODULE - /proc/modules has neither
     * the directory nor the suffix. */
    dot = strrchr(out, '.');
    if (dot != NULL && strcmp(dot, ".o") == 0)
        *dot = '\0';
}

static void
journal_step(const struct vlhe_step *st, FILE *out)
{
    const char *p = st->cmd;
    const char *arg;
    char        name[128];
    int         kind, n;

    (void)out;

    if (strncmp(p, "insmod ", 7) == 0) {
        kind = VLHE_CH_MODULE;
        p += 7;
    } else if (strncmp(p, "modprobe ", 9) == 0) {
        kind = VLHE_CH_MODULE;
        p += 9;
    } else if (strncmp(p, "rmmod ", 6) == 0) {
        kind = VLHE_CH_RMMODULE;
        p += 6;
    } else if (st->kind == VLHE_STEP_DAEMON) {
        kind = VLHE_CH_DAEMON;
    } else {
        return;                 /* nothing that changes state */
    }

    /* The first word after the verb, minus any directory - a module
     * is the same module wherever it was loaded from, and a journal
     * that said /mnt/xfer/vsound.o would not match an undo that says
     * vsound. */
    while (*p == ' ')
        p++;
    arg = strchr(p, ' ');
    n = arg != NULL ? (int)(arg - p) : (int)strlen(p);
    if (n > (int) sizeof name - 1)
        n = sizeof name - 1;
    memcpy(name, p, n);
    name[n] = '\0';
    {
        char *slash = strrchr(name, '/');
        if (slash != NULL)
            memmove(name, slash + 1, strlen(slash + 1) + 1);
    }

    /* A daemon's `name' is its binary; its arguments are its
     * configuration and belong in the detail for the same reason a
     * module's do. */
    if (arg != NULL)
        while (*arg == ' ')
            arg++;

    vlhe_journal_add(kind, name, arg);
}

/* ------------------------------------------------------------------ */
/* Device nodes                                                       */
/* ------------------------------------------------------------------ */

/*
 * MAKE /dev/vdiscN MATCH THE MODULE WE JUST LOADED, AND SWEEP THE
 * ONES THAT NO LONGER DO.
 *
 * THE BUG THIS FIXES, found on 86Box 2026-09-21 and it presented as
 * nothing at all. `/etc/vlhe.conf' said `Major = 60', the plan loaded
 * vdisc at 60, and the nodes on disk were major 63 from an older
 * `load.sh' run. So:
 *
 *   - the module was loaded          (/proc/modules said so)
 *   - vdiscd was running             (its pid file said so)
 *   - /dev/vdisc0 reached major 63, where nothing is registered
 *   - open() returned -ENXIO
 *   - vlhe_drive_count() read that as "no such drive" and stopped
 *   - the CD page said "no virtual drives are available"
 *
 * EVERY LAYER REPORTED CORRECTLY and the machine still could not
 * attach a disc. Nothing in the chain was in a position to say "the
 * node names a major nobody owns", which is what made it four
 * inferences deep to find.
 *
 * `load.sh' NEVER HITS IT because it hardcodes VDISC_MAJOR=63 for
 * both the insmod and the mknod, so its two numbers cannot disagree.
 * The CLI reads the major from config and created no nodes at all -
 * VLHE_STEP_NODE has been defined in vlhe_apply.h since the first
 * commit with nothing ever emitting one.
 *
 * THE POLICY IS load.sh's, DELIBERATELY - lines 1145-1215, which
 * already worked it out including the sweep the user asked for on
 * 2026-09-19 ("we will have to clean up nodes if the majors
 * change"). Restating it in C rather than inventing a second policy:
 *
 *   1. for each drive we are about to have: create the node if it is
 *      absent, REPLACE it if its major is wrong
 *   2. above that count, remove a vdiscN ONLY IF its major is not
 *      ours - a node at the current major is harmless, costs an
 *      inode, and is reused by the next run with more drives
 *   3. /dev/vdiscctl follows /proc/misc, remade when the minor moved
 *
 * WHY 2 MATTERS AND IS NOT TIDYING. A node naming a major something
 * else has since claimed is a live trap: open it and you reach that
 * driver, not us, with nothing to say so. Going from 4 drives to 1
 * leaves vdisc1..3 behind, and if the major also changed they are
 * exactly that.
 *
 * NOT A SHELL COMMAND, for the reason `(stop ...)' is not: the
 * control node's minor is only knowable AFTER the module loads, so
 * this cannot be a string built at plan time. The step carries
 * "(nodes ...)" and the runner calls this.
 *
 * Returns 0, or -1 if anything could not be made. `out' gets a line
 * per change, nothing when there was nothing to do.
 */
static int
make_nodes(int maj, int ndevs, FILE *out)
{
    int i, bad = 0;
    struct stat sb;
    char path[64];

    if (ndevs <= 0)
        return 0;
    /* NO MAJOR TO MAKE THEM AT, AND SAYING SO - design/47 L4. The
     * caller resolves a configured 0 to the major the module took;
     * arriving here with none means the module is not in
     * /proc/devices, and "nothing happened" would hide that. */
    if (maj <= 0) {
        if (out != NULL)
            fprintf(out, "#   vdisc has no block major in /proc/devices -"
                         " is the module loaded? No nodes made\n");
        return -1;
    }

    for (i = 0; i < ndevs && i < VLHE_MAX_DRIVE; i++) {
        int have = -1;

        sprintf(path, "/dev/vdisc%d", i);

        /* lstat, NOT stat - design/47 L2: a symlink here is judged as
         * a symlink, as remove_nodes() and load.sh judge it, not by
         * whatever block device it points at. */
        if (lstat(path, &sb) == 0 && S_ISBLK(sb.st_mode))
            have = (int) major(sb.st_rdev);

        if (have == maj)
            continue;           /* already right */

        if (have >= 0) {
            /*
             * WRONG MAJOR - REPLACE IT. This is the case that cost
             * the evening, and it is silent otherwise: the node
             * exists, it is a block device, and it is useless.
             */
            int have_min = (int) minor(sb.st_rdev);

            if (unlink(path) != 0) {
                if (out != NULL)
                    fprintf(out, "#   %s: cannot remove (major %d)"
                                 " - %s\n", path, have, strerror(errno));
                bad = 1;
                continue;
            }
            if (out != NULL)
                fprintf(out, "#   %s was major %d (stale, not ours) -"
                             " remade at %d\n", path, have, maj);
            /* JOURNALLED, AND NOT REMEMBERED FOR THE UNDO - design/47
             * L1, REVERSED 2026-10-02. L1 made the undo put the
             * replaced node back, "as it was before the apply". The
             * user: "should we just delete that node anyways instead
             * of restoring it back wrong?" - and the /dev/dsp symlink
             * case below (plan_load's prior) had already answered
             * that for a leftover of OUR OWN: restoring it faithfully
             * every cycle converges on a state the machine never had.
             * A /dev/vdiscN at another major is ours - no stock
             * device has that name (Documentation/devices.txt) - from
             * a previous boot's dynamic major or an earlier install,
             * so it is stale, not someone's. The journal records the
             * removal as the sweep below does; the session record
             * carries NO "was", so remove_recorded_nodes() remakes
             * nothing and the unload leaves the path empty. */
            {
                char det[64];
                sprintf(det, "was b %d,%d - stale, not our major",
                        have, have_min);
                vlhe_journal_add(VLHE_CH_RMNODE, path, det);
            }
            {
                char det[64];

                sprintf(det, "b %d,%d", maj, i);
                if (node_record_first(path, det, out) != 0) {
                    bad = 1;
                    continue;
                }
                if (mknod(path, S_IFBLK | 0660, makedev(maj, i)) != 0) {
                    if (out != NULL)
                        fprintf(out, "#   %s: mknod failed - %s\n",
                                path, strerror(errno));
                    node_record_unmade(path);
                    bad = 1;
                    continue;
                }
                if (out != NULL)
                    fprintf(out, "#   %s created (major %d, minor %d)\n",
                            path, maj, i);
                vlhe_journal_add(VLHE_CH_NODE, path, det);
            }
            continue;
        }

        {
            char det[64];

            /* THE SESSION RECORD FIRST, so the unload removes THIS
             * node rather than recomputing the major from a config
             * that may have changed (design/40) - and BEFORE the node,
             * so a crash between the two leaves a record of it
             * (design/54 7h Stage 1.5). */
            sprintf(det, "b %d,%d", maj, i);
            if (node_record_first(path, det, out) != 0) {
                bad = 1;
                continue;
            }
            if (mknod(path, S_IFBLK | 0660, makedev(maj, i)) != 0) {
                if (out != NULL)
                    fprintf(out, "#   %s: mknod failed - %s\n",
                            path, strerror(errno));
                node_record_unmade(path);
                bad = 1;
                continue;
            }
            if (out != NULL)
                fprintf(out, "#   %s created (major %d, minor %d)\n",
                        path, maj, i);
            vlhe_journal_add(VLHE_CH_NODE, path, det);
        }
    }

    /*
     * THE SWEEP. Only ours, only above the count, only when the major
     * is NOT the one we just loaded - see 2 above.
     */
    for (i = ndevs; i < VLHE_MAX_DRIVE; i++) {
        sprintf(path, "/dev/vdisc%d", i);

        if (lstat(path, &sb) != 0 || !S_ISBLK(sb.st_mode))
            continue;
        if ((int) major(sb.st_rdev) == maj)
            continue;           /* harmless, and reusable */

        if (unlink(path) == 0) {
            if (out != NULL)
                fprintf(out, "#   %s removed - major %d is not"
                             " ours\n", path, (int) major(sb.st_rdev));
            {
                char det[64];
                sprintf(det, "was b %d,%d - stale, not our major",
                        (int) major(sb.st_rdev), (int) minor(sb.st_rdev));
                vlhe_journal_add(VLHE_CH_RMNODE, path, det);
            }
        } else {
            if (out != NULL)
                fprintf(out, "#   %s: cannot remove - %s\n",
                        path, strerror(errno));
            bad = 1;
        }
    }

    /*
     * THE CONTROL NODE, whose minor comes from /proc/misc and is
     * assigned at load time - which is the whole reason this is C and
     * not a planned command string.
     */
    {
        FILE *fp = fopen("/proc/misc", "r");
        int   ctl = -1;

        if (fp != NULL) {
            char line[128];

            while (fgets(line, sizeof line, fp) != NULL) {
                int  m;
                char name[64];

                if (sscanf(line, "%d %63s", &m, name) == 2
                    && strcmp(name, "vdiscctl") == 0) {
                    ctl = m;
                    break;
                }
            }
            fclose(fp);
        }

        if (ctl < 0) {
            if (out != NULL)
                fprintf(out, "#   vdiscctl is not in /proc/misc -"
                             " is vdisc loaded?\n");
            bad = 1;
        } else {
            int have = -1;
            int there = (lstat("/dev/vdiscctl", &sb) == 0);

            if (there && S_ISCHR(sb.st_mode)
                && (int) major(sb.st_rdev) == 10)
                have = (int) minor(sb.st_rdev);

            if (have != ctl) {
                /* WHATEVER IS THERE GOES, AND IS NAMED - design/47 L3.
                 * load.sh removes and remakes this node whatever it
                 * is; this removed it only when it was a char device
                 * on major 10, so a plain file or a symlink left
                 * mknod() failing with "File exists" and a step that
                 * did not say why. */
                if (there) {
                    const char *what =
                        S_ISCHR(sb.st_mode) ? "a character device on"
                                              " another major"
                      : S_ISBLK(sb.st_mode) ? "a block device"
                      : S_ISLNK(sb.st_mode) ? "a symlink"
                      : S_ISREG(sb.st_mode) ? "a plain file"
                      : "not a device";
                    if (unlink("/dev/vdiscctl") != 0) {
                        if (out != NULL)
                            fprintf(out, "#   /dev/vdiscctl is %s and"
                                         " cannot be removed - %s\n",
                                    what, strerror(errno));
                        bad = 1;
                        goto ctl_done;
                    }
                    if (out != NULL)
                        fprintf(out, "#   /dev/vdiscctl was %s -"
                                     " replaced\n", what);
                }
                {
                    char det[64];

                    sprintf(det, "c 10,%d", ctl);
                    if (node_record_first("/dev/vdiscctl", det, out) != 0)
                        bad = 1;
                    else if (mknod("/dev/vdiscctl", S_IFCHR | 0660,
                                   makedev(10, ctl)) == 0) {
                        if (out != NULL)
                            fprintf(out, "#   /dev/vdiscctl created"
                                         " (misc minor %d)\n", ctl);
                        vlhe_journal_add(VLHE_CH_NODE, "/dev/vdiscctl",
                                         det);
                    } else {
                        if (out != NULL)
                            fprintf(out, "#   /dev/vdiscctl: mknod"
                                         " failed - %s\n", strerror(errno));
                        node_record_unmade("/dev/vdiscctl");
                        bad = 1;
                    }
                }
            }
        }
    }
ctl_done:

    /*
     * AND THEY ARE THE GROUP'S - design/33 section 3k, ruling 2. On an
     * installed machine the drives and the control node are root:vlhe
     * 0660: the daemon, running as the account, opens /dev/vdiscctl;
     * a user in the group reads a disc; `cdrom' grants nothing for
     * them, by the ruling. Done on every load, so a node the postinst
     * made, or one an older load left root's, comes right too. A
     * portable copy leaves them as mknod made them.
     */
    {
        struct vlhe_account a;

        if (vlhe_apply_daemon_account(&a) == 1) {
            int changed = 0;

            for (i = 0; i <= ndevs && i <= VLHE_MAX_DRIVE; i++) {
                if (i == ndevs || i == VLHE_MAX_DRIVE)
                    strcpy(path, "/dev/vdiscctl");
                else
                    sprintf(path, "/dev/vdisc%d", i);
                if (lstat(path, &sb) != 0
                    || !(S_ISBLK(sb.st_mode) || S_ISCHR(sb.st_mode)))
                    continue;
                if (sb.st_uid == 0 && sb.st_gid == (gid_t) a.gid
                    && (sb.st_mode & 07777) == 0660)
                    continue;
                if (chown(path, 0, (gid_t) a.gid) != 0
                    || chmod(path, 0660) != 0) {
                    if (out != NULL)
                        fprintf(out, "#   %s: could not give it to the %s"
                                     " group - %s\n", path, VLHE_ACCOUNT,
                                strerror(errno));
                    bad = 1;
                    continue;
                }
                changed++;
                if (i == ndevs || i == VLHE_MAX_DRIVE)
                    break;
            }
            if (changed && out != NULL)
                fprintf(out, "#   %d node(s) now root:%s 0660\n", changed,
                        VLHE_ACCOUNT);
        }
    }

    return bad ? -1 : 0;
}

/* MAJOR 14 IS OSS's, and vmidi is a MINOR within it - vmidi has no
 * major of its own (design/32 section 11). Defined here because
 * both remove_nodes() and make_vmidi_node() need it. */
#define VMIDI_MAJOR 14

/*
 * POINT /dev/dsp AT VSOUND - design/38 section 9, and it replaces the
 * whole card shuffle.
 *
 * WHAT IT IS FOR. Programs open "/dev/dsp" by name. Rather than
 * displacing the user's card so vsound can hold that minor - which
 * meant rmmod, modprobe, an ordering rule, and a machine left with no
 * card at all when `modprobe sb' failed for want of its arguments -
 * we leave the card exactly where it is and point the NAME at vsound.
 *
 * THE SAVED STATE CARRIES A SHAPE AS WELL AS A TARGET. /dev/dsp may
 * be a real char node (the stock case, minor 3) or already a symlink
 * (a paid-OSS machine, or our own second run). An unload must restore
 * WHAT IT WAS, not merely something that works - a machine that
 * arrived with /dev/dsp -> /dev/dsp0 should leave the same way. The
 * file holds one line: "node MAJOR MINOR" or "link TARGET".
 *
 * IT IS SAFE TO REPLACE A NODE HERE WHERE THE CDROM PATH REFUSES TO.
 * /dev/cdrom's real-node case is refused because that node IS the
 * user's drive and nothing else names it. /dev/dsp is different: the
 * card keeps its own /dev/dspN, which is where the pump writes, so
 * the device remains reachable under its own name the whole time.
 * Only the generic alias moves.
 */
/*
 * `prior' GETS THE SAME TEXT THE FILE GETS - the caller needs it for
 * the session record (design/40), and computing it twice is how the
 * two would drift.
 */
/*
 * DESCRIBE WHAT IS AT `path', IN THE SESSION FILE'S VOCABULARY.
 *
 * IT NO LONGER SAVES ANYTHING - `dsp.was' went on 2026-09-25 and
 * this is what is left of the function that wrote it. The caller
 * puts `prior' straight into the session record, which is the one
 * place the state is now kept.
 *
 * WHAT WENT WITH THE FILE, and it is the better half of this change:
 *
 *   - THE NO-OVERWRITE GUARD. It refused to write over an existing
 *     `dsp.was' because one file could hold one path's state, and
 *     its comment said "the first save is the one that knows what
 *     the machine looked like before we touched anything". True of
 *     one path and wrong the moment the file was reused for a
 *     second, which is a shape this suspected of stranding three
 *     symlinks before the logs said otherwise.
 *
 *   - THE FAILURE PATH. A write that failed refused the whole
 *     redirect. That guard still exists and is better placed: it is
 *     now `vlhe_session_add()' failing in the caller, which refuses
 *     for the same reason and covers every kind of change rather
 *     than this one.
 *
 * `other' MEANS SOMETHING WE CANNOT REBUILD - a directory, a plain
 * file. The restore leaves those alone rather than guessing.
 */
/*
 * A NODE THE UNDO CANNOT KNOW ABOUT IS NOT MADE - 2026-10-02, the
 * user's question with the transfer volume full: "what happens to the
 * logs since they are on the same volume?" The session file is on it
 * too, and four node sites wrote their record with `(void)'. That was
 * fixed by making the node, recording it, and removing it again if the
 * record did not take.
 *
 * AND NOW THE RECORD COMES FIRST - design/54 7h Stage 1.5, 2026-10-03,
 * the other agent's point and the user's approval: made-then-recorded
 * leaves a window where a crash orphans a node no record names. So the
 * record is written (and fsync()ed - vlhe_session_add()) BEFORE the
 * node: a record that cannot be written refuses the node outright, and
 * a node whose mknod() then fails has its record closed again, so the
 * unload is not sent after something that never existed. (A record for
 * a node a crash prevented is harmless: the unload finds nothing there,
 * which is the wanted end state.) Returns 0 recorded, -1 refused.
 */
/*
 * THE BASELINE'S SEED - design/54 7h Stage 3.2. Called just before the
 * first session record of a path is written, which is before the path
 * changes - so what is there now is how the machine was, unless what is
 * there is VLHE's own leftover. Then the original is INFERRED, NARROWLY:
 *
 *  - a symlink is VLHE's only if its target is one a session file
 *    (live, or filed in sessions/) says VLHE MADE for this path - a user
 *    pointing /dev/dsp at /dev/dsp1 to choose a card is an ordinary
 *    setup and stays FOUND; the original is then the earliest recorded
 *    prior that is not itself VLHE's, or MAKEDEV's fixed numbers
 *    (/dev/dsp 14,3, /dev/dspN 14,16N+3, devices.txt);
 *  - /dev/cdrom is VLHE's when it points at the inner link or a vdisc
 *    node; its original comes from a session file or is UNKNOWN -
 *    per machine, never guessed (7c);
 *  - /dev/vmidi, /dev/vdisc*, /dev/vdiscctl present with a session
 *    record were made by VLHE, so the original is absent.
 */

/* Every session file, oldest first: the filed ones in name order (their
 * names are their times), then the live one. Calls `fn' per record of
 * `path'. */
static void
session_files_each(const char *path,
                   void (*fn)(const struct vlhe_sess_rec *, void *),
                   void *ctx)
{
    const char *dir = vlhe_session_dir();
    char **name = NULL;
    int nn = 0, cap = 0, i, k, n;
    DIR *d;
    struct dirent *e;
    struct vlhe_sess_rec *rec;

    d = opendir(dir);
    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            char **g;

            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;               /* README, ., .. */
            if (nn == cap) {
                cap = cap ? cap * 2 : 16;
                g = (char **) realloc(name, cap * sizeof *name);
                if (g == NULL)
                    break;
                name = g;
            }
            name[nn] = (char *) malloc(strlen(dir) + strlen(e->d_name) + 2);
            if (name[nn] == NULL)
                break;
            sprintf(name[nn], "%s/%s", dir, e->d_name);
            nn++;
        }
        closedir(d);
    }
    /* A SIMPLE SORT - a few dozen names at most. */
    for (i = 1; i < nn; i++)
        for (k = i; k > 0 && strcmp(name[k - 1], name[k]) > 0; k--) {
            char *t = name[k];

            name[k] = name[k - 1];
            name[k - 1] = t;
        }
    for (i = 0; i <= nn; i++) {
        rec = vlhe_session_read_path(i < nn ? name[i] : vlhe_session_path(),
                                     &n);
        for (k = 0; k < n; k++)
            if (strcmp(rec[k].what, path) == 0)
                fn(&rec[k], ctx);
        free(rec);
        if (i < nn)
            free(name[i]);
    }
    free(name);
}

struct infer_ctx {
    const char *now_target;     /* the link's target, or NULL       */
    int  is_cdrom;
    int  made_now;              /* a record says VLHE made now_target */
    int  any_node;              /* a NODE record exists for the path  */
    char first_prior[VLHE_PATH_MAX]; /* earliest prior not VLHE's    */
};

static int
target_is_cdrom_ours(const char *t)
{
    return strcmp(t, vdiscd_ctl_cdrom()) == 0
           || strncmp(t, "/dev/vdisc", 10) == 0;
}

static void
infer_visit(const struct vlhe_sess_rec *r, void *vctx)
{
    struct infer_ctx *c = (struct infer_ctx *) vctx;
    char made[VLHE_PATH_MAX];

    if (r->kind == VLHE_SESS_NODE)
        c->any_node = 1;
    note_made(r->note, made, sizeof made);
    if (c->now_target != NULL && made[0] != '\0') {
        const char *b = strrchr(made, '/');

        if (strcmp(c->now_target, made) == 0
            || (b != NULL && strcmp(c->now_target, b + 1) == 0))
            c->made_now = 1;
    }
    /* THE EARLIEST PRIOR THAT IS NOT ITSELF ONE OF VLHE'S LINKS - row
     * 79's chain of records each holding the previous leftover is
     * skipped over to the state before it. */
    if (c->first_prior[0] == '\0' && r->kind == VLHE_SESS_LINK) {
        const char *p = r->prior[0] != '\0' ? r->prior : "absent";
        int ours = 0;

        if (strncmp(p, "link ", 5) == 0) {
            if (c->is_cdrom)
                ours = target_is_cdrom_ours(p + 5);
            else if (c->now_target != NULL
                     && strcmp(p + 5, c->now_target) == 0)
                ours = 1;
        }
        if (!ours) {
            strncpy(c->first_prior, p, sizeof c->first_prior - 1);
            c->first_prior[sizeof c->first_prior - 1] = '\0';
        }
    }
}

int
vlhe_apply_baseline_infer(const char *path, const char *now, char *orig,
                          size_t max)
{
    struct infer_ctx c;
    int vlhe_node;

    memset(&c, 0, sizeof c);
    c.is_cdrom = strcmp(path, "/dev/cdrom") == 0;
    if (strncmp(now, "link ", 5) == 0)
        c.now_target = now + 5;
    session_files_each(path, infer_visit, &c);

    strncpy(orig, now, max - 1);
    orig[max - 1] = '\0';

    if (c.now_target != NULL) {
        int ours = c.is_cdrom ? target_is_cdrom_ours(c.now_target)
                              : c.made_now;

        if (!ours)
            return VLHE_BASE_FOUND;
        if (c.first_prior[0] != '\0') {
            strncpy(orig, c.first_prior, max - 1);
            orig[max - 1] = '\0';
            return VLHE_BASE_INFERRED;
        }
        if (c.is_cdrom) {
            strncpy(orig, "unknown", max - 1);
            return VLHE_BASE_UNKNOWN;
        }
        if (strcmp(path, "/dev/dsp") == 0) {
            strncpy(orig, "node 14 3", max - 1);
            return VLHE_BASE_INFERRED;
        }
        {
            int nn;
            char tail;

            if (sscanf(path, "/dev/dsp%d%c", &nn, &tail) == 1
                && nn >= 0 && nn < 16) {
                sprintf(orig, "node 14 %d", 16 * nn + 3);
                return VLHE_BASE_INFERRED;
            }
        }
        strncpy(orig, "unknown", max - 1);
        return VLHE_BASE_UNKNOWN;
    }

    vlhe_node = strcmp(path, "/dev/vmidi") == 0
                || strncmp(path, "/dev/vdisc", 10) == 0;
    if (vlhe_node && c.any_node
        && (strncmp(now, "node ", 5) == 0 || strncmp(now, "block ", 6) == 0)) {
        strncpy(orig, "absent", max - 1);
        return VLHE_BASE_INFERRED;
    }
    return VLHE_BASE_FOUND;
}

/*
 * DOES ANYTHING ANSWER AT A dsp LINK'S TARGET? - design/54 7h Stage 3.5.
 * 1 nothing (missing, ENODEV - unless i810_audio or trident, whose ENODEV
 * means all channels busy - or ENXIO), 0 something does (it opens, or
 * says busy), -1 cannot tell: opening it could load a module
 * (vlhe_modconf.h), so it is not opened. O_NONBLOCK, closed at once.
 */
static int
dsp_target_dangling(const char *target)
{
    char full[VLHE_PATH_MAX + 8];
    struct stat sb;
    int fd;

    if (target[0] != '/')
        sprintf(full, "/dev/%.*s", VLHE_PATH_MAX - 8, target);
    else
        strcpy(full, target);
    if (stat(full, &sb) != 0)
        return 1;               /* missing */
    if (!S_ISCHR(sb.st_mode) || (int) major(sb.st_rdev) != 14)
        return 0;               /* not a sound node: not ours to judge */
    if (vlhe_modconf_open_would_load((int) minor(sb.st_rdev), NULL, 0))
        return -1;
    fd = open(full, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    if (errno == ENXIO)
        return 1;
    if (errno == ENODEV)
        return (vlhe_status_module_loaded("i810_audio")
                || vlhe_status_module_loaded("trident")) ? 0 : 1;
    return 0;                   /* EBUSY, EAGAIN, EWOULDBLOCK: a driver */
}

static void
baseline_seed(const char *path, const char *vsnode, FILE *out)
{
    char now[VLHE_PATH_MAX], st[VLHE_PATH_MAX];
    int  how, have;

    have = vlhe_baseline_get(path, NULL);
    if (have != 0) {
        if (have < 0 && out != NULL)
            fprintf(out, "#   %s: cannot read - %s not added to it\n",
                    vlhe_baseline_path(), path);
        return;                 /* written once, or unreadable */
    }
    vlhe_baseline_describe(path, now, sizeof now);
    how = vlhe_apply_baseline_infer(path, now, st, sizeof st);

    /*
     * FOUND, BUT A dsp LINK THAT MAY BE VLHE'S OWN - 7h Stage 3.5, the
     * second agent's catch. With the folder lost there is no session
     * file to attribute it by, so without this the leftover would become
     * the machine's "original" and never be mentioned again. Marked when
     * nothing answers at its target, or when it points at the very node
     * vsound has now (7d: "no record, already points at vsound") - which
     * answers, because vsound is loaded by the time the link is seeded.
     * Reported until the user restores the stock node or keeps it.
     */
    if (how == VLHE_BASE_FOUND && strncmp(now, "link ", 5) == 0
        && strncmp(path, "/dev/dsp", 8) == 0) {
        const char *t = now + 5;
        const char *vb = vsnode != NULL ? strrchr(vsnode, '/') : NULL;

        if ((vsnode != NULL && (strcmp(t, vsnode) == 0
                                || (vb != NULL && strcmp(t, vb + 1) == 0)))
            || dsp_target_dangling(t) == 1)
            how = VLHE_BASE_DANGLING;
    }

    if (vlhe_baseline_add(path, st, how) < 0) {
        if (out != NULL)
            fprintf(out, "#   cannot write %s - %s's original is not in"
                         " the baseline (the session file still has"
                         " it)\n", vlhe_baseline_path(), path);
    } else if (how == VLHE_BASE_DANGLING && out != NULL) {
        fprintf(out, "#   NOTE: %s is %s - it may be an earlier VLHE"
                     " load's leftover or your own setup; recorded as"
                     " found and marked to check.%s\n", path, now,
                how_fix());
    } else if (how == VLHE_BASE_UNKNOWN && out != NULL) {
        fprintf(out, "#   NOTE: %s is %s - an earlier VLHE load's"
                     " leftover, and what was there before it is not"
                     " known (it is never guessed); recorded as"
                     " UNKNOWN. After the unload it is left as it is.%s\n",
                path, now, how_fix());
    } else if (how != VLHE_BASE_FOUND && out != NULL) {
        fprintf(out, "#   baseline: %s was VLHE's own leftover (%s) -"
                     " recorded its original as %s (%s)\n", path, now,
                st, vlhe_baseline_how_word(how));
    }
}

static int
node_record_first(const char *path, const char *det, FILE *out)
{
    baseline_seed(path, NULL, out);
    if (vlhe_session_add(VLHE_SESS_NODE, path, det, NULL) == 0)
        return 0;
    if (out != NULL)
        fprintf(out, "#   cannot record %s in %s (disk full?) - NOT"
                     " made: the unload could not have known about it\n",
                path, vlhe_session_path());
    return -1;
}

/* The node was recorded and then not made - close its record (the
 * newest OPEN one for the path) so nothing waits on it. */
static void
node_record_unmade(const char *path)
{
    struct vlhe_sess_rec *rec;
    int n, i;

    rec = vlhe_session_read_all(&n);
    for (i = n - 1; i >= 0; i--)
        if (rec[i].kind == VLHE_SESS_NODE
            && rec[i].state == VLHE_SESS_OPEN
            && strcmp(rec[i].what, path) == 0) {
            (void) vlhe_session_mark(i, VLHE_SESS_UNDONE,
                                     "never made - mknod failed");
            break;
        }
    free(rec);
}

/*
 * PUT A NODE BACK AS IT WAS - design/55 recommendation 2, fixing R10
 * (2026-10-04). `recorded' is the state the baseline or the session
 * kept; when it carries a mode and owner (vlhe_state_perm()) the node
 * gets exactly those. A record from before 2026-10-04 has neither and
 * keeps the old 0666 (0660 for a block node): guessing an owner would
 * be worse than what every unload did until now, and the next Load
 * records the real ones. mknod()'s mode is masked by the umask, so the
 * mode is set again afterwards; lchown() because nothing here should
 * follow a link, even one root just made the name of.
 */
/*
 * IS THIS A PATH AND A STATE VLHE MAKES? - design/55 recommendation 2
 * (R2/R3, section 6 rows 12-13), 2026-10-04.
 *
 * The session file and the baseline sit in a directory the `vlhe'
 * account owns (or a portable folder), and root acts on what they say
 * at every unload, Undo and repair, and unattended at boot: unlink the
 * path, then symlink or mknod what was there. With nothing checked, a
 * planted record made root delete any file, or mknod a raw disk under
 * a name with any mode. So before acting, both halves are held to what
 * VLHE itself ever records:
 *
 *   paths   /dev/dsp, /dev/dspN, /dev/vmidi[N], /dev/vdiscctl,
 *           /dev/vdiscN, /dev/cdrom[N] - nothing else, no "..".
 *   nodes   char 14 at a dsp minor for /dev/dsp*, char 14 for
 *           /dev/vmidi*, char 10 for /dev/vdiscctl; block only for
 *           /dev/vdiscN, at the configured vdisc major or 44 or 63
 *           (the module's default and the postinst's).
 *   links   a /dev path, a bare name (relative, in /dev), or vdiscd's
 *           own inner link - never with "..".
 *
 * Returns 1 to go ahead; 0 refused, said on `out'.
 */
static int
name_is(const char *path, const char *stem, int digits_needed)
{
    size_t      n = strlen(stem);
    const char *p;

    if (strncmp(path, stem, n) != 0)
        return 0;
    p = path + n;
    if (*p == '\0')
        return !digits_needed;
    if (strlen(p) > 2)
        return 0;
    for (; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static int
recorded_ok(const char *what, const char *state, FILE *out)
{
    int  maj = -1, min = -1, ok = 0, test = 0;
    int  dsp, vmidi, ctl, vdisc, cdrom;
    char as_dev[VLHE_PATH_MAX];
    const char *name, *td;

    if (what == NULL || state == NULL || strstr(what, "..") != NULL)
        goto refuse;

    /*
     * THE HOST TESTS' STAND-IN FOR /dev. They cannot touch /dev, so they
     * restore files in a scratch directory named by VLHE_TEST_DEVDIR; a
     * name directly inside it is judged as if it were in /dev, and its
     * NAME is not held to the list (the tests use names like `a').
     * Every STATE check still applies. Root never sees this variable -
     * vlhe_self_root_env(), design/55 recommendation 3 - so it cannot
     * widen what root acts on.
     */
    name = what;
    td = getenv("VLHE_TEST_DEVDIR");
    if (td != NULL && td[0] != '\0' && strncmp(what, td, strlen(td)) == 0
        && what[strlen(td)] == '/' && strchr(what + strlen(td) + 1, '/') == NULL
        && strlen(what + strlen(td)) < sizeof as_dev - 8) {
        sprintf(as_dev, "/dev%s", what + strlen(td));
        name = as_dev;
        test = 1;
    }

    dsp   = name_is(name, "/dev/dsp", 0);
    vmidi = name_is(name, "/dev/vmidi", 0);
    ctl   = strcmp(name, "/dev/vdiscctl") == 0;
    vdisc = name_is(name, "/dev/vdisc", 1);
    cdrom = name_is(name, "/dev/cdrom", 0);
    if (!dsp && !vmidi && !ctl && !vdisc && !cdrom && !test)
        goto refuse;

    if (state[0] == '\0' || strcmp(state, "absent") == 0)
        return 1;

    if (strncmp(state, "link ", 5) == 0) {
        const char *t = state + 5;
        const char *in = vdiscd_ctl_cdrom();

        if (strstr(t, "..") != NULL || t[0] == '\0')
            goto refuse;
        if (strncmp(t, "/dev/", 5) == 0 || strchr(t, '/') == NULL
            || (in != NULL && strcmp(t, in) == 0))
            return 1;
        goto refuse;
    }

    /*
     * TWO SPELLINGS OF A NODE, AND BOTH ARE VLHE'S - 86Box 2026-10-04.
     * The baseline and the card's records say "node M m" / "block M m";
     * a node the LOAD MADE is recorded as "c M,m" / "b M,m"
     * (make_nodes(), make_vmidi_node()). Accepting only the first
     * refused every node VLHE made, and each unload left /dev/vmidi and
     * the vdisc nodes FAILED - "Unfinished unload" straight after one.
     * The same checks apply to both spellings.
     */
    if (sscanf(state, "node %d %d", &maj, &min) == 2
        || sscanf(state, "c %d,%d", &maj, &min) == 2)
        ok = (dsp && maj == 14 && min >= 3 && (min - 3) % 16 == 0)
             || (vmidi && maj == VMIDI_MAJOR)
             || (ctl && maj == 10);
    else if ((sscanf(state, "block %d %d", &maj, &min) == 2
              || sscanf(state, "b %d,%d", &maj, &min) == 2) && vdisc) {
        struct vlhe_modopts cd;

        ok = maj == 44 || maj == 63
             || (vlhe_modopts(&cd) == 0 && cd.major > 0 && maj == cd.major);
    }
    if (ok && min >= 0 && min < 256)
        return 1;

refuse:
    if (out != NULL)
        fprintf(out, "#   %.200s: the record says \"%.100s\" - not a path"
                     " and state VLHE makes, so it is left alone\n",
                what != NULL ? what : "?", state != NULL ? state : "?");
    return 0;
}

static int
remake_node(const char *path, int blk, int maj, int min,
            const char *recorded)
{
    int  mode;
    long uid, gid;
    int  have = vlhe_state_perm(recorded, &mode, &uid, &gid);
    int  m = have ? mode : (blk ? 0660 : 0666);

    if (mknod(path, (blk ? S_IFBLK : S_IFCHR) | (m & 0777),
              makedev(maj, min)) != 0)
        return -1;
    if (have)
        (void) lchown(path, (uid_t) uid, (gid_t) gid);
    (void) chmod(path, m);
    return 0;
}

static void
describe_dsp_state(const char *path, char *prior, size_t plen)
{
    struct stat sb;
    char  desc[VLHE_PATH_MAX + 32];

    if (prior == NULL || plen == 0)
        return;

    if (lstat(path, &sb) != 0)
        strcpy(desc, "absent");
    else if (S_ISLNK(sb.st_mode)) {
        char t[VLHE_PATH_MAX];
        int  n = readlink(path, t, sizeof t - 1);

        if (n < 0)
            n = 0;
        t[n] = '\0';
        sprintf(desc, "link %.200s", t);
    } else if (S_ISCHR(sb.st_mode)) {
        /* WITH ITS MODE AND OWNER - design/55 R10: the restore remade
         * Corel's root:audio 0660 node as 0666. */
        char perm[64];

        vlhe_state_perm_suffix((int) (sb.st_mode & 07777),
                               (long) sb.st_uid, (long) sb.st_gid, perm);
        sprintf(desc, "node %d %d%s",
                (int) major(sb.st_rdev), (int) minor(sb.st_rdev), perm);
    } else
        strcpy(desc, "other");

    strncpy(prior, desc, plen - 1);
    prior[plen - 1] = '\0';
}

/*
 * GIVE WHATEVER IS AT /dev/dsp A SECOND NAME BEFORE WE TAKE IT.
 *
 * THE HOLE THE USER FOUND, 2026-09-25: "How do we talk to the real
 * card if it was dev/dsp?" On a stock single-card machine the card IS
 * minor 3 and `/dev/dsp' is its ONLY name - MAKEDEV creates just
 * `dsp' and `dsp1'. Replacing that name with a symlink does not move
 * the card, it ORPHANS it: the driver is still at minor 3 and nothing
 * can reach it, so `vsoundd -d' has nowhere to point.
 *
 * THE /dev/cdrom PRECEDENT DOES NOT COVER THIS, and assuming it did
 * is what missed it. There the drive keeps `/dev/hdc' underneath and
 * `/dev/cdrom' is an ALIAS, so redirecting costs nothing. `/dev/dsp'
 * is the card's PRIMARY node, not an alias for one.
 *
 * `/dev/dspN' IS THE RIGHT SECOND NAME, and it is why the paid OSS
 * makes `dsp0' beside `dsp': `dsp0' is the un-aliased name for minor
 * 3, which is what lets `dspdefault -> dsp0' mean anything. We make
 * the same name for the same reason.
 *
 * A NO-OP WHEN THE NODE ALREADY EXISTS, and on most machines it will
 * not: MAKEDEV stops at `dsp1'.
 */
/* name_the_card() recorded the node itself, before touching it (1: a
 * stock node replaced, 2: a new node) - so the caller does not record
 * it again. 0: it changed nothing, and the caller records the name it
 * found. design/54 7h Stage 1.5. */
static int g_card_recorded;

/* "node MAJ MIN mode 0660 uid 0 gid 29" for a character node's stat -
 * the prior a session record keeps, with the mode and owner since
 * design/55 R10. Truncated to `len' rather than overrun. */
static void
node_state_of(const struct stat *sb, char *out, size_t len)
{
    char d[96], perm[64];

    vlhe_state_perm_suffix((int) (sb->st_mode & 07777),
                           (long) sb->st_uid, (long) sb->st_gid, perm);
    sprintf(d, "node %d %d%s", (int) major(sb->st_rdev),
            (int) minor(sb->st_rdev), perm);
    strncpy(out, d, len - 1);
    out[len - 1] = '\0';
}

static int
name_the_card(const char *path, int vsidx, FILE *out, char *out_node,
              size_t max, char *stock_prior, size_t stock_len)
{
    struct stat sb, card;
    int idx, minor_have;
    char newname[VLHE_PATH_MAX];

    out_node[0] = '\0';
    g_card_recorded = 0;

    /* NOT A LINK ALREADY - if /dev/dsp is a symlink, whatever it
     * names is reachable under that name and nothing is orphaned. */
    if (lstat(path, &sb) != 0 || !S_ISCHR(sb.st_mode))
        return 0;
    card = sb;      /* its mode and owner, for the card's new name */

    if ((int) major(sb.st_rdev) != 14)
        return 0;               /* not a sound device - leave it */

    minor_have = (int) minor(sb.st_rdev);
    if (minor_have < 3 || (minor_have - 3) % 16 != 0)
        return 0;               /* not a dsp minor */
    idx = (minor_have - 3) / 16;

    sprintf(newname, "/dev/dsp%d", idx);

    /*
     * THE CANONICAL NAME MAY BE THE PATH WE ARE ABOUT TO TAKE, and
     * then it is no name at all - found on target 2026-09-25, row 77.
     *
     * With `ProgramsUse = /dev/dsp2' the card at minor 35 has
     * `idx == 2', so `newname' is `/dev/dsp2' - the very path the
     * symlink is about to replace. The early return below then
     * reported it as the card's node, the link took the path, and
     * the card had NO reachable name left. A game configured for
     * dsp2 met the pump there instead and was refused: the user,
     * "doing dsp2/dsp2 failed again something blocked doom".
     *
     * `/dev/dsp' DOES NOT HIT THIS, because its canonical name is
     * `/dev/dsp0' - a different path - which is why the single-card
     * case worked throughout and this one never could.
     *
     * SO FIND A FREE NAME INSTEAD. Any unused `/dev/dspN' serves:
     * the number carries no meaning beyond the minor behind it, and
     * the pump is TOLD which node to write to rather than guessing.
     */
    /*
     * THE CANONICAL NAME MAY BE THE PATH WE ARE ABOUT TO TAKE, and
     * then it is no name at all - row 77. A card at minor 35 has
     * `idx == 2', so `newname' is `/dev/dsp2', the very path the
     * symlink replaces. The card would be left unreachable.
     *
     * THE FREE NAME IS `vsound's INDEX + 1', AND IT IS ARITHMETIC
     * RATHER THAN A SEARCH - the user, 2026-09-25: "playthrough
     * already knows all the dsps before load. vsound gets the next
     * free. so the dsp after vsound should be free."
     *
     * WHY THAT HOLDS, from `sound_core.c:91': with `index < 0'
     * `__sound_insert_unit()' walks up from `low' and stops at the
     * FIRST hole. So vsound took the lowest free slot, and nothing
     * can sit between it and the next registered card - a gap there
     * is precisely the gap vsound would have taken.
     *
     * TWO WRONG ANSWERS CAME FIRST, BOTH MINE. The canonical name,
     * which is the path being taken; then a search for a node that
     * does not EXIST, which refuses on any ordinary machine because
     * MAKEDEV creates dsp0..dsp15 - the user: "there is a differece
     * between existing and taken."
     *
     * SO: COMPUTE. DO NOT SEARCH - design/36 row 87, 2026-09-26.
     *
     * **THIS BLOCK USED TO SEARCH, AND THE COMMENT ABOVE ALREADY
     * SAID IT SHOULD NOT.** The decision was recorded on 2026-09-25
     * and the implementation below stayed a loop over
     * `vsidx + 1 .. 7' testing each candidate with `open()'. The
     * user found it on target: *"if vsound got the dsp3 that most
     * likely means dsp4 is free because vsound got assigned the
     * first free dsp"*. Right, and the reasoning is three paragraphs
     * up - the fix went into the comment and not the code.
     *
     * AND THE SEARCH COULD NEVER HAVE SUCCEEDED ANYWAY. It accepted
     * `ENXIO' alone, but an unregistered dsp minor is answered by
     * `soundcore_open()', which returns **`-ENODEV`**
     * (`sound_core.c:382`). So every free candidate was rejected as
     * "busy, or something else" and the load refused with *"no free
     * /dev/dspN answers above vsound"* on a machine where four names
     * were free. Two independent defects, either one fatal.
     *
     * `CLAUDE.md' SAYS TO MATCH `ENXIO' AND THAT IS NOT WRONG - it
     * came from the 2026-09-25 driver sweep, which asked "is this
     * card BUSY" (`EBUSY`, `EAGAIN`, `EWOULDBLOCK`). This asks "is
     * anything REGISTERED", and soundcore answers that one with
     * ENODEV. Different question, different errno.
     *
     * THE OPEN STAYS AS A WARNING, NOT A VETO. If something does
     * answer at `vsidx + 1' the arithmetic is wrong and we want to
     * know - but refusing on it is what broke this, so it reports
     * and proceeds.
     */
    if (strcmp(newname, path) == 0) {
        int  k = vsidx + 1;
        int  fd;
        char cand[VLHE_PATH_MAX];
        char why[120];

        newname[0] = '\0';

        /* 8, NOT 16 - MEASURED 2026-09-25. `register_sound_dsp' is
         * `sound_insert_unit(..., 3, 131)', so the last minor it
         * allocates is 115 = `/dev/dsp7'. A name above that could
         * never be associated with the card. Filling the space with
         * four Ensoniq cards made `insmod vsound.o' fail rc=1, which
         * is the ceiling observed rather than read. */
        if (k < 8) {
            sprintf(cand, "/dev/dsp%d", k);

            /* IT CANNOT BE `path'. `path' resolves to the card, whose
             * index is `idx', and vsound sits at `vsidx' - if those
             * were equal the card and the mixer would be one device.
             * Checked rather than assumed, because a stale
             * /proc/vsound would make it so. */
            if (strcmp(cand, path) == 0) {
                if (out != NULL)
                    fprintf(out, "#   %s is both the card and"
                                 " vsound+1 - NOT redirecting\n", cand);
            } else if (vlhe_modconf_open_would_load(16 * k + 3, why,
                                                    sizeof why)) {
                /* AN ALIAS WOULD PUT A DRIVER HERE - CLAUDE.md section 5.
                 * Taken, the conservative answer: opening it to look
                 * would load that driver, and borrowing the name would
                 * collide with it the moment anything else opened it. */
                if (out != NULL)
                    fprintf(out, "#   %s: an alias would load a driver"
                                 " there (%s) - not used for the card\n",
                            cand, why);
            } else {
                fd = open(cand, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    close(fd);
                    if (out != NULL)
                        fprintf(out, "#   NOTE: %s answers, so"
                                     " something is registered where"
                                     " vsound+1 should be free -"
                                     " using it anyway\n", cand);
                }
                strcpy(newname, cand);
            }
        }
        if (newname[0] == '\0') {
            /* THE MESSAGE CHANGED WITH THE SEARCH, 2026-09-26. It
             * said "no free /dev/dspN answers above vsound", which
             * described the loop that is gone - and which was
             * printed on a machine with four free names because the
             * errno test was wrong. The only ways here now are
             * vsound holding the LAST slot, or the card and vsound+1
             * being the same path. */
            if (out != NULL) {
                if (vsidx + 1 >= 8)
                    fprintf(out, "#   vsound is /dev/dsp%d, the"
                                 " last slot the kernel allocates, so"
                                 " there is no name left for the card"
                                 " - NOT redirecting %s, because the"
                                 " card would become unreachable\n",
                            vsidx, path);
                else
                    fprintf(out, "#   no name is free for the card"
                                 " - NOT redirecting %s, because it"
                                 " would become unreachable\n", path);
            }
            return -1;
        }

        /*
         * THE STOCK NODE IS REMOVED SO THE mknod BELOW CAN POINT
         * THE NAME AT OUR CARD'S MINOR - and this is the one place
         * here that deletes something we did not create, so it is
         * worth being plain about.
         *
         * IT IS SAFE BECAUSE THE PROBE JUST PROVED NOTHING IS
         * BEHIND IT. `ENXIO' means the minor is registered to no
         * driver, so the node is a name MAKEDEV left lying about
         * rather than a device anyone can use. Opening it before
         * this returns the same ENXIO.
         *
         * AND THE UNLOAD PUTS THE STOCK NODE BACK SINCE 2026-09-26 -
         * design/36 row 80, which this used to describe as a known
         * gap. The numbers are captured HERE, in the only moment
         * they exist: after this unlink the node is gone, and its
         * minor cannot be recovered from anything on the machine.
         *
         * `stock_prior' travels to the caller, which writes it as
         * the session record's `prior' in the same `node MAJ MIN'
         * vocabulary the link records use. `remove_recorded_nodes()'
         * reads it back and recreates what MAKEDEV had.
         */
        if (stock_prior != NULL && stock_len > 0
            && lstat(newname, &sb) == 0 && S_ISCHR(sb.st_mode))
            node_state_of(&sb, stock_prior, stock_len);

        /* RECORDED BEFORE THE STOCK NODE GOES - design/54 7h Stage 1.5.
         * After the unlink its numbers exist nowhere else, so a crash
         * between the two used to lose them for good. Kept OPEN even
         * if the mknod below then fails: the unload recreates the
         * stock node from it. */
        if (node_record_first(newname,
                              stock_prior != NULL ? stock_prior : "",
                              out) != 0)
            return -1;
        g_card_recorded = 1;

        (void) unlink(newname);
    } else if (lstat(newname, &sb) == 0) {
        /*
         * ALREADY NAMED - AND THIS BRANCH USED TO LOSE THE NODE.
         *
         * It reported the name and returned, recording NOTHING. The
         * caller then wrote a session line with an EMPTY prior:
         *
         *     node /dev/dsp0 | - | UNDONE | -
         *
         * and `remove_recorded_nodes()' unlinked `/dev/dsp0' on the
         * unload with nothing to put back. The undo's own summary
         * still said "the machine is as it was before the apply",
         * which was false.
         *
         * **MEASURED, NOT INFERRED, 2026-09-28.** The user created
         * `/dev/dsp0' by hand, loaded, unloaded, and it was gone;
         * `tests/logs/2026-09-28-dsp0-deleted-by-unload/' has four
         * cycles, each with `remove node /dev/dsp0' and NO matching
         * create, and an empty prior on every session line.
         *
         * AND IT IS THE ORDINARY PATH, NOT AN EDGE CASE. MAKEDEV
         * creates `dsp0..dsp15', so on a stock machine this branch
         * is the one that runs - every load/unload cycle quietly
         * deleted a node MAKEDEV had made.
         *
         * SO RECORD IT, exactly as the branch above records the
         * stock node it unlinks, and in the same `node MAJ MIN'
         * vocabulary `remove_recorded_nodes()' reads back.
         */
        if (stock_prior != NULL && stock_len > 0 && S_ISCHR(sb.st_mode))
            node_state_of(&sb, stock_prior, stock_len);

        /*
         * AND CHECK IT IS THE CARD'S MINOR, which `lstat' alone does
         * not - it proves only that SOMETHING is there. A node at
         * the wrong minor would be handed to the pump as the card
         * and silently pumped into the wrong device.
         *
         * REPORTED, NOT REFUSED. `make_dsp_node()' takes the same
         * shape for vsound's own node: say what is wrong, give the
         * command that fixes it, and let the user decide. Refusing
         * here would block a load over a node the user may have put
         * there deliberately.
         */
        if (out != NULL && S_ISCHR(sb.st_mode)
            && ((int) major(sb.st_rdev) != 14
                || (int) minor(sb.st_rdev) != minor_have))
            fprintf(out, "#   %s exists and is %d,%d - the card is"
                         " on minor %d. USING IT ANYWAY, but the pump"
                         " may be writing to the wrong device.\n"
                         "#   fix by hand if it is wrong:"
                         " rm %s; mknod %s c 14 %d\n",
                    newname, (int) major(sb.st_rdev),
                    (int) minor(sb.st_rdev), minor_have,
                    newname, newname, minor_have);

        strncpy(out_node, newname, max - 1);
        out_node[max - 1] = '\0';
        return 0;
    }

    /* NOTHING WAS THERE - record it before making it (7h Stage 1.5). */
    if (!g_card_recorded) {
        if (node_record_first(newname, "", out) != 0)
            return -1;
        g_card_recorded = 2;
    }
    /* THE CARD'S OWN MODE AND OWNER, NOT 0666 - design/55 R10. Its new
     * name reaches the same hardware as the /dev/dsp node it had, so it
     * gets the same permissions: Corel's root:audio 0660 stays 0660,
     * where 0666 let every user open the card, recording included. */
    if (mknod(newname, S_IFCHR | (card.st_mode & 0777),
              makedev(14, minor_have)) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: mknod failed - %s. The card at"
                         " minor %d would be left with no name once"
                         " %s becomes a link\n",
                    newname, strerror(errno), minor_have, path);
        if (g_card_recorded == 2)
            node_record_unmade(newname);    /* nothing was there */
        return -1;
    }
    (void) lchown(newname, card.st_uid, card.st_gid);
    (void) chmod(newname, card.st_mode & 07777);

    strncpy(out_node, newname, max - 1);
    out_node[max - 1] = '\0';
    if (out != NULL)
        fprintf(out, "#   %s created (c 14 %d) - the card keeps a"
                     " name of its own\n", newname, minor_have);
    return 0;
}

/*
 * POINT THE USER'S DEVICE AT VSOUND.
 *
 * `path' IS THE NAME THEIR PROGRAMS OPEN - `ProgramsUse', /dev/dsp
 * unless they said otherwise. IT IS NOT NECESSARILY /dev/dsp, and
 * that is the user's point, 2026-09-25: someone with two working
 * cards has already configured their players for, say, /dev/dsp2.
 * Redirecting /dev/dsp would mix a name they never open, while
 * vsoundd held dsp2 exclusively and BLOCKED the one they do - worse
 * than doing nothing.
 *
 * SETTING IT TO THE SAME DEVICE AS THE CARD NEEDS NO SPECIAL CASE:
 * name_the_card() gives whatever is there a second name first, so
 * the pump can still reach it afterwards.
 */
/*
 * STILL TO UNDO - OPEN or FAILED. design/40 and the session file's own
 * header say FAILED means "the next unload retries it", and every
 * restore took OPEN only: a record that failed once was never tried
 * again, and with design/54 7h Stage 2a refusing a Load over it the
 * machine was stuck (86Box 2026-10-03, test 5). UNDONE and CONFLICT are
 * final and never touched.
 */
static int
rec_pending(const struct vlhe_sess_rec *r)
{
    return r->state == VLHE_SESS_OPEN || r->state == VLHE_SESS_FAILED;
}

/*
 * Does the live session hold an OPEN link record for `path'? 1 if so -
 * and when `target' is given and differs from the record's `made', the
 * record's note is brought up to date, so the restore (7h Stage 1.3)
 * does not take VLHE's own re-pointed link for someone else's change.
 */
static int
session_link_open(const char *path, const char *target)
{
    struct vlhe_sess_rec *rec;
    int n, i, found = 0;

    rec = vlhe_session_read_all(&n);
    for (i = 0; i < n; i++)
        if (rec[i].kind == VLHE_SESS_LINK
            && rec[i].state == VLHE_SESS_OPEN
            && strcmp(rec[i].what, path) == 0) {
            found = 1;
            break;
        }
    if (found && target != NULL) {
        char *m = strstr(rec[i].note, "made ");

        if (m != NULL && strncmp(m + 5, target, strlen(target)) != 0) {
            char note[sizeof rec[i].note];
            size_t keep = (size_t) (m - rec[i].note);

            if (keep + 5 + strlen(target) < sizeof note) {
                memcpy(note, rec[i].note, keep);
                sprintf(note + keep, "made %s", target);
                (void) vlhe_session_mark(i, VLHE_SESS_OPEN, note);
            }
        }
    }
    free(rec);
    return found;
}

/* Point `path' at `target' in one step: a new link beside it, renamed
 * over it, so `path' is never absent. 0, or -1 with the reason said. */
static int
relink_atomic(const char *path, const char *target, FILE *out)
{
    char tmp[VLHE_PATH_MAX];

    if (strlen(path) + sizeof ".vlhe-new" > sizeof tmp)
        return -1;
    strcpy(tmp, path);
    strcat(tmp, ".vlhe-new");
    (void) unlink(tmp);
    if (symlink(target, tmp) != 0 || rename(tmp, path) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: could not point it at %s - %s\n",
                    path, target, strerror(errno));
        (void) unlink(tmp);
        vlhe_journal_failed(VLHE_CH_NODE, path, strerror(errno));
        return -1;
    }
    return 0;
}

static int
link_dsp_set(const char *path, int idx, FILE *out)
{
    char target[VLHE_PATH_MAX];
    char cardnode[VLHE_PATH_MAX];
    char prior[VLHE_PATH_MAX];
    /* WHAT MAKEDEV HAD AT THE BORROWED NAME, so the unload can put
     * it back - design/36 row 80. Empty when no name was borrowed,
     * or when nothing was there to begin with. */
    char stockprior[64];
    struct stat sb;

    prior[0] = '\0';
    stockprior[0] = '\0';

    if (idx < 0 || path == NULL || path[0] == '\0')
        return 0;               /* nothing to point at, or nowhere to */

    sprintf(target, "/dev/dsp%d", idx);

    /* ALREADY RIGHT - vsound landed on the very node they name, so
     * opening it reaches the mixer with nothing to redirect. On a
     * single-card machine with the default this is the common case. */
    if (strcmp(path, target) == 0) {
        if (out != NULL)
            fprintf(out, "#   %s IS vsound - nothing to redirect\n",
                    path);
        return 0;
    }

    /*
     * A REPEAT LOAD IN THE SAME SESSION - design/54 D24, 7h Stage 1.2,
     * 2026-10-03. This step runs on every Load, deliberately (design/36
     * row 86), and a second Load with vsound already up found the
     * session's OWN symlink, recorded it as the prior state - a second
     * record, whose undo would put the symlink back - and printed row
     * 79's "an earlier unload did not finish". The user's screenshot of
     * 2026-10-03 shows it.
     *
     * So when the session already holds an OPEN record for this path,
     * the first record has the original and nothing is recorded again:
     * pointing at vsound already - nothing to do; pointing anywhere
     * else - someone changed it while VLHE was loaded, the same case a
     * restore calls a CONFLICT: say so, journal it, point it back.
     */
    if (session_link_open(path, target)) {
        char now[VLHE_PATH_MAX];
        const char *base = strrchr(target, '/');
        int  n;

        now[0] = '\0';
        if (lstat(path, &sb) == 0 && S_ISLNK(sb.st_mode)
            && (n = (int) readlink(path, now, sizeof now - 1)) > 0)
            now[n] = '\0';
        else
            now[0] = '\0';
        if (strcmp(now, target) == 0
            || (base != NULL && strcmp(now, base + 1) == 0)) {
            if (out != NULL)
                fprintf(out, "#   %s already points at %s (this"
                             " session) - nothing to do\n", path, target);
            return 0;
        }
        if (out != NULL)
            fprintf(out, "#   NOTE: %s is now %s - changed while VLHE was"
                         " loaded; pointing it back at %s. The unload"
                         " still restores what was there first.\n",
                    path, now[0] != '\0' ? now : "not a symlink", target);
        if (relink_atomic(path, target, out) != 0)
            return -1;
        {
            char det[VLHE_PATH_MAX + 64];

            sprintf(det, "-> %.200s (re-pointed: it had been changed)",
                    target);
            vlhe_journal_add(VLHE_CH_NODE, path, det);
        }
        return 0;
    }

    /* THE BASELINE FIRST - before anything at `path' moves, 7h 3.2;
     * `target' is vsound's node, for 3.5's DANGLING mark. */
    baseline_seed(path, target, out);

    /* THE CARD FIRST - before the name stops pointing at it. */
    if (name_the_card(path, idx, out, cardnode, sizeof cardnode,
                      stockprior, sizeof stockprior) != 0)
        return -1;

    /*
     * RECORD BEFORE CHANGING, AND REFUSE IF IT CANNOT BE RECORDED.
     *
     * The same guard `link_cdrom_set()' has, and for the same
     * reason: a redirect we cannot undo is worse than no redirect.
     * It used to be `save_dsp_state()' failing to write `dsp.was';
     * that file is gone and the session record replaces it, so the
     * guard moves here rather than disappearing with the file.
     *
     * BEFORE, NOT AFTER. A record written after the symlink would
     * leave a window where the link is moved and nothing says what
     * it was - which is exactly the state that needed a hand repair
     * on 2026-09-25.
     */
    describe_dsp_state(path, prior, sizeof prior);

    /*
     * IS WHAT WE FOUND OUR OWN LEFTOVER? - design/36 row 79.
     *
     * A `prior' of `link /dev/dspN' means this path was ALREADY a
     * symlink into the dsp space when we arrived, and **MAKEDEV
     * NEVER MAKES ONE**: `/dev/dsp' is char 14,3 and its siblings
     * are 14,19 / 14,35 and so on (`Documentation/devices.txt:420`,
     * and the user's own `dsp.was' recorded `node 14 35'). So the
     * only thing that puts a symlink there is us, and finding one
     * means a previous session did not finish putting it back.
     *
     * RECORDING IT WOULD MAKE THE DAMAGE PERMANENT, and that is
     * measured rather than feared. On 2026-09-26 four clean cycles
     * in one boot each recorded `prior = link /dev/dsp7' and each
     * undo faithfully restored it - a chain of balanced, honest
     * undos converging on a state the machine never had. The user
     * confirmed it: "that would be a leftover from a previous
     * state", and repaired it by hand.
     *
     * WHY NOT THE cdrom GUARD. `link_cdrom_set()' refuses when the
     * session file already holds a record for the path
     * (`st.saved[0] != '\0'`), which protects WITHIN one session.
     * It cannot protect across them: once an unload marks its
     * records UNDONE the next load finds nothing and records
     * whatever is there. The pulled session file shows exactly that
     * - three `link /dev/dsp' records, all UNDONE, all
     * `prior = link /dev/dsp7'.
     *
     * WE WARN RATHER THAN REPAIR, deliberately. Putting it right
     * means guessing which minor the stock node had, and guessing
     * at device numbers is how the original breakage got worse.
     * The user can act on a warning; the software cannot safely act
     * on a guess. design/36 row 79 weighed both and chose this.
     *
     * AND WE STILL RECORD IT AND CARRY ON. Refusing the load over a
     * stale link would be worse: the machine works, the redirect is
     * correct, and only the restore target is wrong - so the user
     * gets a working system plus a message, not a refusal.
     */
    /*
     * ONLY WHEN THE BASELINE HAS NO ENTRY - 2026-10-04, the user's
     * part-5 run printed this under baseline_seed()'s own note and the
     * two disagreed. Once seeded, the baseline has already judged the
     * link: DANGLING says "maybe a leftover, maybe yours" and the
     * Needs attention row puts it right; FOUND is someone's own setup,
     * where "nothing but VLHE puts a symlink there" is false. So this
     * speaks only when the baseline could not be read or written - and
     * names the front end's fix rather than MAKEDEV by hand.
     */
    if (strncmp(prior, "link /dev/dsp", 13) == 0 && out != NULL
        && vlhe_baseline_get(path, NULL) <= 0) {
        fprintf(out, "#   NOTE: %s was ALREADY a symlink to %s\n",
                path, prior + 5);
        fprintf(out, "#   It may be an earlier VLHE load's leftover;"
                     " the unload will put that symlink back.\n");
        if (how_fix()[0] != '\0')
            fprintf(out, "#   To fix: unload, then%s\n", how_fix());
        else
            fprintf(out, "#   To fix: unload, then recreate the node"
                         " (see /dev/MAKEDEV), then load again.\n");
    }

    {
        char note[96];

        /*
         * THE CARD'S NEW NAME GOES IN THE NOTE, because the pump
         * needs it and nothing else will know it.
         *
         * When the pick and the redirect are the same path - both
         * settings on `/dev/dsp2' - `name_the_card()' has just given
         * that card a DIFFERENT name, and `/dev/dsp2' now resolves
         * to vsound. `probe_card()' reads this back so the pump
         * writes to the card rather than into the mixer. Row 77.
         */
        /* AND WHAT WE MADE - `made <target>', design/54 7h Stage 1.3:
         * the restore compares the link with it, and anything else is
         * someone's change (a CONFLICT), not ours to undo. Bounded so
         * the three parts fit the 96-byte note. */
        if (cardnode[0] != '\0')
            sprintf(note, "was %.30s; card at %.22s; made %.22s",
                    prior[0] != '\0' ? prior : "unknown", cardnode, target);
        else
            sprintf(note, "was %.50s; made %.30s",
                    prior[0] != '\0' ? prior : "unknown", target);
        if (vlhe_session_add(VLHE_SESS_LINK, path, prior, note) != 0) {
            if (out != NULL)
                fprintf(out, "#   cannot record the change in %s"
                             " - NOT changing %s, because it could not"
                             " be put back\n",
                        vlhe_session_path(), path);
            return -1;
        }
    }

    /*
     * AND RECORD THE BORROWED NAME AS A NODE OF OURS - design/36
     * row 80.
     *
     * `name_the_card()' made `cardnode' with `mknod', so the unload
     * must remove it; that part already worked, because a NODE
     * record is what `remove_recorded_nodes()' acts on. What was
     * missing is `prior': MAKEDEV may have had a node at that name,
     * and unlinking it without recording its numbers left `/dev'
     * permanently short of a filename after a load/unload cycle.
     *
     * NOTHING COULD OPEN IT EITHER WAY - no driver sits on that
     * minor, which is why the probe returned ENXIO and why the
     * unlink was safe. The cost was a missing NAME, not a missing
     * device. It is still a departure from "the machine is as it
     * was", which is what the journal claims.
     *
     * AN EMPTY `stockprior' IS THE ORDINARY CASE and means there was
     * nothing there before - a machine with only MAKEDEV's two dsp
     * nodes borrowing `/dev/dsp4', say. The record is still written,
     * so the node we made is still removed; only the recreate step
     * is skipped.
     */
    if (cardnode[0] != '\0' && !g_card_recorded)
        baseline_seed(cardnode, NULL, out);
    if (cardnode[0] != '\0' && !g_card_recorded
        && vlhe_session_add(VLHE_SESS_NODE, cardnode, stockprior,
                            NULL) != 0 && out != NULL)
        fprintf(out, "#   warning: %s could not be recorded in %s"
                     " - the unload will not remove it\n",
                cardnode, vlhe_session_path());

    if (lstat(path, &sb) == 0 && unlink(path) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: cannot remove - %s\n",
                    path, strerror(errno));
        return -1;
    }

    if (symlink(target, path) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: symlink failed - %s\n",
                    path, strerror(errno));
        vlhe_journal_failed(VLHE_CH_NODE, path, strerror(errno));
        return -1;
    }

    if (out != NULL)
        fprintf(out, "#   %s -> %s, so programs opening %s reach"
                     " the mixer\n", path, target, path);
    {
        char det[VLHE_PATH_MAX + 32];

        sprintf(det, "-> %.200s", target);
        vlhe_journal_add(VLHE_CH_NODE, path, det);
    }

    /*
     * AND THE SESSION FILE, WHICH IS WHAT THE UNDO READS.
     *
     * The journal line above is for a person; this one is for
     * `plan_unload()', and it carries the PATH - the field `dsp.was'
     * never had, and the reason the unload used to ask the config
     * which node to restore. design/40.
     *
     * AFTER the change succeeded, never before: a record of what was
     * ATTEMPTED would have the undo try to reverse something that
     * never happened.
     */
    return 0;
}

/*
 * POINT `/dev/cdrom' AT THE VIRTUAL DRIVE, SAVING WHAT IT WAS.
 *
 * THE OPTION IS OFF BY DEFAULT AND THIS RUNS ONLY WHEN IT IS ON,
 * because the path is outside anything we own.
 *
 * WE ARE REDIRECTING SOMETHING THAT ALREADY WORKS. Measured on
 * tests/vm/fresh.img: stock Corel HAS `/dev/cdrom', a symlink to
 * `/dev/hdb', made at first boot by `/sbin/fooze' from the
 * `[cdrom.1]' stanza in /etc/devices. KsCD hardcodes that path. So
 * this takes over a link the user's REAL DRIVE depends on, which is
 * why the saved target is written to a FILE before anything changes.
 *
 * A REAL DEVICE NODE IS REFUSED, NOT REPLACED - the user's call. The
 * refusal names a free `/dev/cdromN' so there is a way forward.
 *
 * Returns 0, or -1 when the link could not be made.
 */
/* THE DRIVE /dev/cdrom SHOULD REACH, as the drive state recorded it
 * (vdiscd writes `Cdrom = N' when the CD page's radio moves it), else
 * drive 0 - where the link has always pointed.
 *
 * AND ONLY A DRIVE THIS LOAD MAKES - design/54 D20, found by the user
 * 2026-10-03: "4 vdics /dev/cdrom->/dev/vdisc3, unloaded swapped to 1
 * vdisc and loaded /dev/cdrom->/dev/vdisc3". The record was checked
 * against the maximum (8), so a lowered Drives count left /dev/cdrom
 * on a node that no longer answers. A drive past the count gives 0
 * here and the record is not touched, so raising the count again
 * brings the choice back. */
static int
cdrom_drive_recorded(void)
{
    struct vlhe_conf c;
    struct vlhe_modopts cd;
    int n, ndevs = VLHE_MAX_DRIVE;

    c.n = 0;
    if (vlhe_conf_read(&c, vlhe_conf_drives_path()) < 0)
        return 0;
    n = vlhe_conf_get_int(&c, "Drives", "Cdrom", 0);
    if (vlhe_modopts(&cd) == 0 && cd.ndevs > 0 && cd.ndevs < ndevs)
        ndevs = cd.ndevs;
    return (n >= 0 && n < ndevs) ? n : 0;
}

/*
 * /dev/cdrom -> <run dir>/cdrom -> /dev/vdiscN - 2026-10-03, the user's
 * design ("build it that way"). The outer link is made HERE, once, as
 * root; the inner one names the drive and lives where the daemons'
 * account can move it, so the CD page swaps drives live through vdiscd
 * (`cdrom N') without root or an unload. The inner link is made first,
 * so /dev/cdrom never dangles; an outer link still pointing straight
 * at /dev/vdiscN from an older load is re-pointed through it.
 */
static int
link_cdrom_set(FILE *out)
{
    struct vlhe_cdrom_link st;
    int drv = cdrom_drive_recorded();

    if (vlhe_cdrom_link(&st) != 0)
        return -1;

    if (st.kind == VLHE_CDROM_SYMLINK || st.kind == VLHE_CDROM_ABSENT) {
        if (vdiscd_ctl_set_cdrom(drv) != 0) {
            if (out != NULL)
                fprintf(out, "#   %s: cannot point it at /dev/vdisc%d -"
                             " %s; /dev/cdrom not changed\n",
                        vdiscd_ctl_cdrom(), drv, strerror(errno));
            return -1;
        }
    }

    if (st.kind == VLHE_CDROM_NODE || st.kind == VLHE_CDROM_OTHER) {
        if (out != NULL) {
            fprintf(out, "#   /dev/cdrom is %s, not a symlink -"
                         " left alone\n",
                    st.kind == VLHE_CDROM_NODE ? "a real device node"
                                               : "not a link or a node");
            if (st.free_name[0] != '\0')
                fprintf(out, "#   %s is free; a player that can be"
                             " pointed at it will reach the virtual"
                             " drive\n", st.free_name);
        }
        /* NOT A FAILURE. We declined deliberately, and the machine is
         * exactly as it was - which is the outcome the refusal is
         * for. */
        return 0;
    }

    if (st.is_ours && strcmp(st.target, vdiscd_ctl_cdrom()) == 0) {
        /*
         * SEEDED HERE TOO - 2026-10-04, row 142 part 5. With the folder
         * lost and the link surviving a reset, this return came before
         * the seed below, so /dev/cdrom got no baseline line at all and
         * the unload left it at a dead link with nothing on record.
         * Seeding it now records what can be known: UNKNOWN when no
         * session file says what was there (never guessed), the real
         * original when one does - and nothing when the baseline
         * already has it, which is every ordinary Load-again.
         */
        baseline_seed("/dev/cdrom", NULL, out);
        if (out != NULL)
            fprintf(out, "#   /dev/cdrom already points at %s -> "
                         "/dev/vdisc%d\n", st.target, drv);
        return 0;
    }

    /*
     * SAVE BEFORE CHANGING, AND ONLY IF WE HAVE NOT ALREADY.
     *
     * An apply run twice must not overwrite the ORIGINAL target with
     * our own - that would make the restore a no-op and lose the
     * user's real drive for good. st.saved being non-empty means a
     * previous run recorded it and this one must not.
     */
    /*
     * THE RECORD GOES IN BEFORE THE LINK MOVES, and a failure to
     * write it REFUSES the change.
     *
     * REFUSING IS THE SAFE DIRECTION. A redirect we cannot undo is
     * exactly the state the user described running into by hand -
     * "I forgot it was set and had to swap it back" - and doing it
     * deliberately would be worse.
     *
     * `cdrom.was' IS GONE, 2026-09-25 - the user: "86box is the only
     * machine carrying a cdrom.was so there is no point keeping it
     * just for that one machine." The session file holds the same
     * fact with the path attached, which is what lets one restore
     * path serve both /dev/cdrom and the dsp nodes.
     *
     * ONLY IF NOTHING IS RECORDED YET. `st.saved' non-empty means an
     * earlier load already noted the user's real drive; recording
     * our own symlink over it would make the restore a no-op and
     * lose that drive for good.
     */
    baseline_seed("/dev/cdrom", NULL, out);     /* 7h Stage 3.2 */
    if (st.saved[0] == '\0') {
        char prior[VLHE_PATH_MAX + 16];

        if (st.kind == VLHE_CDROM_ABSENT)
            strcpy(prior, "absent");
        else
            sprintf(prior, "link %.200s", st.target);

        if (vlhe_session_add(VLHE_SESS_LINK, "/dev/cdrom",
                             prior, NULL) != 0) {
            if (out != NULL)
                fprintf(out, "#   cannot record the change in %s"
                             " - NOT changing the link, because it"
                             " could not be put back\n",
                        vlhe_session_path());
            return -1;
        }
    }

    if (st.kind == VLHE_CDROM_SYMLINK && unlink("/dev/cdrom") != 0) {
        if (out != NULL)
            fprintf(out, "#   /dev/cdrom: cannot remove - %s\n",
                    strerror(errno));
        return -1;
    }

    if (symlink(vdiscd_ctl_cdrom(), "/dev/cdrom") != 0) {
        if (out != NULL)
            fprintf(out, "#   /dev/cdrom: symlink failed - %s\n",
                    strerror(errno));
        vlhe_journal_failed(VLHE_CH_NODE, "/dev/cdrom", strerror(errno));
        return -1;
    }

    if (out != NULL)
        fprintf(out, "#   /dev/cdrom -> %s -> /dev/vdisc%d (was %s)\n",
                vdiscd_ctl_cdrom(), drv,
                st.target[0] != '\0' ? st.target : "absent");
    {
        char det[VLHE_PATH_MAX + 32];

        sprintf(det, "was %.200s", st.target[0] != '\0' ? st.target
                                                        : "absent");
        vlhe_journal_add(VLHE_CH_NODE, "/dev/cdrom", det);
    }

    return 0;
}

/*
 * PUT /dev/dsp BACK - design/38 section 9 and 9g.
 *
 * RESTORES THE SHAPE, NOT JUST A WORKING PATH. The saved line says
 * whether it was a char node or a symlink and what it named, so a
 * machine that arrived with /dev/dsp -> /dev/dsp0 leaves that way and
 * one that arrived with a node at 14,3 gets a node at 14,3.
 *
 * AND IF IT MOVED UNDER US, IT IS LEFT AS FOUND - design/54 7h, the
 * user's decision 5, 2026-10-03, which supersedes the decision of
 * 2026-09-25 recorded here before ("AND IF IT MOVED UNDER US, ASK",
 * through a callback nothing ever registered, defaulting to RESTORE).
 * "Moved" now means it no longer points at the target the Load
 * recorded (`made' in the record's note) - so it is not a link left on
 * vsound's node, which was the reason the old default restored
 * regardless. Someone pointed it elsewhere on purpose; the restore
 * leaves it, journals a CONFLICT, and the caller marks the record
 * CONFLICT - final, so the session is still filed.
 */

/*
 * `saved' IS HANDED IN, NOT READ FROM A FILE - 2026-09-25, and
 * `dsp.was' is gone with it.
 *
 * THE CALLER ALREADY HAS IT. `(dsprestore)' walks the session
 * records to find WHICH path to restore, and the same record carries
 * WHAT it was - so re-opening a file to learn the second half was
 * always a second source for one fact. That is the shape of the bug
 * this whole design exists to remove.
 *
 * AND IT RETIRES THE NO-OVERWRITE GUARD. `save_dsp_state()' refused
 * to write over an existing `dsp.was' because one file could hold
 * only one path's state; with a record per path there is nothing to
 * protect and nothing to collide.
 */
static void shell_restore(FILE *fp, const char *path, const char *prior);

static int
link_dsp_restore(const char *path, const char *saved, const char *made,
                 FILE *out)
{
    char  line[VLHE_PATH_MAX + 32];
    char  now[VLHE_PATH_MAX];
    struct stat sb;
    int   ours = 0;

    if (saved == NULL)
        return 0;               /* we never changed it */

    strncpy(line, saved, sizeof line - 1);
    line[sizeof line - 1] = '\0';

    if (line[0] == '\0' || strcmp(line, "other") == 0) {
        /* Nothing we know how to put back. Say so and keep the file,
         * which is the only record of what was there. */
        if (out != NULL)
            fprintf(out, "#   %s: saved state is \"%s\" -"
                         " left alone\n", path, line);
        return 0;
    }

    /* ONLY WHAT VLHE MAKES - recorded_ok(), design/55 recommendation 2. */
    if (!recorded_ok(path, line, out)) {
        vlhe_journal_failed(VLHE_CH_RMNODE, path,
                            "the record is not a path and state VLHE makes");
        return -1;
    }

    /* IS IT STILL WHAT WE LEFT? A symlink into the dsp space that we
     * made. Anything else means something moved it. */
    now[0] = '\0';
    if (lstat(path, &sb) == 0 && S_ISLNK(sb.st_mode)) {
        int n = readlink(path, now, sizeof now - 1);

        if (n < 0)
            n = 0;
        now[n] = '\0';
        if (made != NULL && made[0] != '\0') {
            /* EXACTLY WHAT THE LOAD MADE - 7h Stage 1.3. A bare
             * basename is accepted too, for a link a shell made
             * relative (`ln -s dsp1 /dev/dsp'). */
            const char *base = strrchr(made, '/');

            ours = strcmp(now, made) == 0
                   || (base != NULL && strcmp(now, base + 1) == 0);
        } else if (strncmp(now, "/dev/dsp", 8) == 0) {
            /* A RECORD FROM BEFORE `made' WAS WRITTEN - the old,
             * looser test, so a load made by an older build still
             * unloads as it did. */
            ours = 1;
        }
    }

    if (!ours) {
        char why[VLHE_PATH_MAX + 64];

        if (out != NULL)
            fprintf(out, "#   %s is now %s, not the %s VLHE made -"
                         " someone changed it while loaded; left as it"
                         " is (CONFLICT)\n", path,
                    now[0] != '\0' ? now : "not a symlink",
                    (made != NULL && made[0] != '\0') ? made
                        : "/dev/dsp link");
        sprintf(why, "now %.100s, not what VLHE made; was %.80s before",
                now[0] != '\0' ? now : "not a symlink", line);
        vlhe_journal_conflict(VLHE_CH_RMNODE, path, why);
        return 2;
    }

    /* DECIDED - IT WILL RUN, so the command is printed now and not
     * before: a CONFLICT, or nothing saved to put back, printed an
     * `rm -f' that never ran (86Box, 2026-10-03). */
    if (out != NULL) {
        shell_restore(out, path, line);
        fflush(out);
    }

    if (lstat(path, &sb) == 0 && unlink(path) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: cannot remove - %s\n",
                    path, strerror(errno));
        vlhe_journal_failed(VLHE_CH_RMNODE, path, strerror(errno));
        return -1;
    }

    if (strncmp(line, "link ", 5) == 0) {
        if (symlink(line + 5, path) != 0) {
            if (out != NULL)
                fprintf(out, "#   %s: cannot restore link to"
                             " %s - %s\n", path, line + 5,
                        strerror(errno));
            vlhe_journal_failed(VLHE_CH_RMNODE, path,
                                strerror(errno));
            return -1;
        }
        if (out != NULL)
            fprintf(out, "#   %s -> %s (restored)\n",
                    path, line + 5);
    } else if (strncmp(line, "node ", 5) == 0) {
        int maj = 0, min = 0;

        sscanf(line + 5, "%d %d", &maj, &min);
        if (remake_node(path, 0, maj, min, line) != 0) {
            if (out != NULL)
                fprintf(out, "#   %s: cannot restore node"
                             " %d,%d - %s\n", path, maj, min,
                        strerror(errno));
            vlhe_journal_failed(VLHE_CH_RMNODE, "/dev/dsp",
                                strerror(errno));
            return -1;
        }
        if (out != NULL)
            fprintf(out, "#   %s restored (c %d,%d)\n",
                    path, maj, min);
    }
    /* "absent" falls through: it was not there before, and the unlink
     * above has put it back to not being there. */

    {
        char det[VLHE_PATH_MAX + 32];

        sprintf(det, "back to %.200s", line);
        vlhe_journal_add(VLHE_CH_RMNODE, path, det);
    }

    /* THE CALLER MARKS THE RECORD - it knows the index, and it is
     * the one place that can tell UNDONE from FAILED. There is no
     * file to unlink any more. Returns 0 restored, -1 FAILED, 2
     * CONFLICT (left as found, journalled above). */
    return 0;
}

/* `made <target>' out of a link record's note, into `out' - empty for a
 * record written before 7h Stage 1.3. */
static void
note_made(const char *note, char *out, size_t max)
{
    const char *m = strstr(note, "made ");
    size_t n = 0;

    out[0] = '\0';
    if (m == NULL)
        return;
    m += 5;
    while (m[n] != '\0' && m[n] != ';' && m[n] != ' ' && n < max - 1)
        n++;
    memcpy(out, m, n);
    out[n] = '\0';
}

/*
 * PUT `/dev/cdrom' BACK, and this is the half the feature exists for.
 *
 * ONLY IF IT STILL POINTS AT US. A user who repointed it by hand
 * since made a deliberate choice, and stamping over that would be the
 * same class of mistake as removing a node we did not create.
 *
 * THE SAVED TARGET COMES OFF DISK, not from memory, so an unload
 * after a reboot - or one run by the init script rather than by the
 * GUI that made the change - still restores correctly.
 */
static int
link_cdrom_restore(FILE *out)
{
    struct vlhe_cdrom_link st;

    if (vlhe_cdrom_link(&st) != 0)
        return -1;

    if (st.saved[0] == '\0')
        return 0;               /* we never changed it */

    if (!st.is_ours) {
        /*
         * SOMETHING ELSE OWNS IT NOW - the user's own hand, or
         * `/sbin/fooze' after a hardware change, which rewrites this
         * link from /etc/devices and has every right to.
         *
         * SAID RATHER THAN SILENTLY SKIPPED, and the saved file is
         * left in place: it is the only record of what the link was
         * before we touched it.
         */
        if (out != NULL)
            fprintf(out, "#   /dev/cdrom points at %s, not at ours"
                         " - left alone (saved: %s)\n",
                    st.target[0] != '\0' ? st.target : "nothing",
                    st.saved);
        return 0;
    }

    /* THE SAVED TARGET IS A RECORD TOO - recorded_ok(), design/55
     * recommendation 2. */
    {
        char ls[VLHE_PATH_MAX + 8];

        sprintf(ls, "link %.*s", VLHE_PATH_MAX - 8, st.saved);
        if (!recorded_ok("/dev/cdrom", ls, out)) {
            vlhe_journal_failed(VLHE_CH_RMNODE, "/dev/cdrom",
                                "the record is not a target VLHE makes");
            return -1;
        }
    }

    if (unlink("/dev/cdrom") != 0) {
        if (out != NULL)
            fprintf(out, "#   /dev/cdrom: cannot remove - %s\n",
                    strerror(errno));
        vlhe_journal_failed(VLHE_CH_RMNODE, "/dev/cdrom",
                            strerror(errno));
        return -1;
    }

    if (symlink(st.saved, "/dev/cdrom") != 0) {
        if (out != NULL)
            fprintf(out, "#   /dev/cdrom: cannot restore to %s -"
                         " %s\n", st.saved, strerror(errno));
        vlhe_journal_failed(VLHE_CH_RMNODE, "/dev/cdrom",
                            strerror(errno));
        return -1;
    }

    if (out != NULL)
        fprintf(out, "#   /dev/cdrom -> %s (restored)\n", st.saved);
    {
        char det[VLHE_PATH_MAX + 32];

        sprintf(det, "back to %.200s", st.saved);
        vlhe_journal_add(VLHE_CH_RMNODE, "/dev/cdrom", det);
    }

    /*
     * CLOSE THE RECORD, AND ONLY ONCE THE RESTORE SUCCEEDED - a
     * record left OPEN would have the next unload try again, and one
     * closed too early would lose the fact that it had not worked.
     *
     * THIS REPLACES `remove(savefile)'. `cdrom.was' is gone
     * (2026-09-25); marking the record UNDONE is the same statement,
     * except that it survives as evidence instead of vanishing, and
     * `vlhe_session_rotate()' needs every line UNDONE before it can
     * file the session.
     *
     * BY PATH RATHER THAN BY INDEX, because this function does not
     * know which index it was written at.
     */
    {
        int n;
        struct vlhe_sess_rec *rec = vlhe_session_read_all(&n);
        int i;

        for (i = n - 1; i >= 0; i--)
            if (rec[i].kind == VLHE_SESS_LINK
                && rec_pending(&rec[i])
                && strcmp(rec[i].what, "/dev/cdrom") == 0) {
                (void) vlhe_session_mark(i, VLHE_SESS_UNDONE, NULL);
                break;
            }
        free(rec);
    }
    return 0;
}

/*
 * REMOVE THE NODES WE MADE - the other half of make_nodes().
 *
 * WHY THIS EXISTS AT ALL, and an earlier reading of it was WRONG. A
 * comparison against load.sh/unload.sh on 2026-09-23 noted that
 * neither script removes nodes, and concluded the plan should not
 * either - "a node at the current major is harmless and is reused by
 * the next run", which make_nodes() itself argues.
 *
 * THE SCRIPTS NEVER REACH THE CASE. load.sh hardcodes one major
 * (44 originally, 63 since 60-63 were found to be the reserved
 * range), so its node set never varies and nothing accumulates. The
 * GUI lets the user CHANGE the major and the drive count, so it can.
 *
 * AND "HARMLESS" IS A JUDGEMENT MADE FROM THE NEXT LOAD'S POINT OF
 * VIEW. It assumes there is a next load. The user's standard, and it
 * is the right one: "the computer especially in portable mode should
 * be left in the state it was prior ... We dont want to flood a users
 * machine with nodes." A portable user unpacks a folder, tries it,
 * and deletes the folder - there is no next load, and what is left
 * behind is ten device nodes naming a driver that is gone.
 *
 * WHAT ACCUMULATES, worked out rather than assumed: NOT one node per
 * major tried - a node is ONE major/minor pair, so changing the major
 * REPLACES /dev/vdisc0 rather than adding to it. The count is bounded
 * by the NAMES: vdisc0..7, vdiscctl, vmidi. Ten, worst case, and the
 * drive-count case is the one that reaches it - going from 4 drives
 * to 1 leaves vdisc1..3, which make_nodes()'s sweep deliberately
 * KEEPS because the major still matches.
 *
 * WE REMOVE ONLY WHAT WE WOULD HAVE MADE, and leave anything else:
 * a node at a major that is not ours was not ours to create and is
 * not ours to delete. That is the same principle as the card module
 * (an unknown holder of device 0 is refused, not unloaded) and as
 * /dev/cdrom (a real device node is refused, not replaced).
 *
 * EVERY REMOVAL IS JOURNALLED, and that is the point rather than a
 * side effect. vlhe_journal_end() prints "the machine is as it was
 * before the apply" on an undo run - and until this existed that
 * claim was FALSE, because the nodes were still there. The journal
 * was asserting something the teardown did not deliver.
 *
 * Returns 0, or -1 if something could not be removed - which is
 * exactly the case vlhe_journal_failed() exists to record, and which
 * leaves the verdict saying so.
 */
/*
 * REMOVE THE NODES THIS SESSION RECORDED, BY PATH, and return how
 * many records it acted on.
 *
 * TRIED BEFORE remove_nodes()'s config-derived sweep. A record names
 * the node the LOAD actually made, so it cannot be aimed at a user's
 * own device by a config that changed in between - which is the
 * whole point of design/40 and the bug that stranded three symlinks
 * on 2026-09-25.
 *
 * `want_midi' selects the family, because the plan removes the disc
 * nodes and the MIDI node in separate steps and each must close only
 * its own records.
 */
static int
remove_recorded_nodes(int want_midi, FILE *out)
{
    struct vlhe_sess_rec *rec;
    int n, i, did = 0, failed = 0;

    rec = vlhe_session_read_all(&n);
    if (n < 0) {
        /* UNREADABLE IS NOT "NOTHING RECORDED" - design/47 section 5:
         * (dsprestore) says so and this did not. The sweep by major
         * still runs, which is the right fallback; it just says why. */
        if (out != NULL)
            fprintf(out, "#   %s: cannot read - the undo list is"
                         " unavailable, sweeping by major instead\n",
                    vlhe_session_path());
        return 0;
    }
    if (n == 0)
        return 0;               /* nothing recorded */

    for (i = n - 1; i >= 0; i--) {
        int is_midi;
        struct stat sb;

        if (rec[i].kind != VLHE_SESS_NODE
            || !rec_pending(&rec[i]))
            continue;

        /*
         * `/dev/dspN' IS NOT OURS TO REMOVE HERE - design/36 row 80.
         *
         * The card's borrowed name is a NODE record like the others,
         * but it is handled by `(dsprestore)', which is in every
         * unload plan THAT HAS SOUND IN SCOPE (since 2026-09-30 - it
         * used to be in every unload plan, and a MIDI-scoped restart
         * tearing down /dev/dsp was the result; see plan_unload()).
         * This split is by "vmidi" or not, so a dsp record would fall
         * to the CD pass - and that step is only added when CD is in
         * scope. Excluding it by name keeps one owner for the node
         * rather than two that must agree about ordering, and leaves
         * the record OPEN across a scoped unload for the sound-scoped
         * one to restore.
         */
        if (strncmp(rec[i].what, "/dev/dsp", 8) == 0)
            continue;

        is_midi = (strstr(rec[i].what, "vmidi") != NULL);
        if (is_midi != (want_midi != 0))
            continue;

        /* ONLY A NAME VLHE MAKES - recorded_ok(), design/55 rec. 2. A
         * planted record would otherwise have root unlink any path.
         * Kept FAILED, so it stays visible rather than silently done. */
        if (!recorded_ok(rec[i].what, rec[i].prior, out)) {
            /* IN THE JOURNAL TOO - a refusal is a change not undone,
             * and the UNDO block listed none of six (86Box
             * 2026-10-04), only the totals line. */
            vlhe_journal_failed(VLHE_CH_RMNODE, rec[i].what,
                                "not a path and state VLHE makes - left alone");
            (void) vlhe_session_mark(i, VLHE_SESS_FAILED,
                                     "not a path VLHE makes - left alone");
            failed++;
            continue;
        }

        if (lstat(rec[i].what, &sb) != 0) {
            /* ALREADY GONE, which IS the wanted end state - so the
             * record closes rather than staying open for a retry
             * that would find nothing either. */
            (void) vlhe_session_mark(i, VLHE_SESS_UNDONE, "already gone");
            did++;
            continue;
        }

        if (unlink(rec[i].what) == 0) {
            int omaj, omin;

            if (out != NULL)
                fprintf(out, "#   %s removed\n", rec[i].what);
            vlhe_journal_add(VLHE_CH_RMNODE, rec[i].what,
                             rec[i].prior[0] != '\0' ? rec[i].prior : NULL);

            /* A NODE THE LOAD REPLACED DOES NOT COME BACK - design/47
             * L1, reversed 2026-10-02: a wrong-major /dev/vdiscN is a
             * stale node of ours and make_nodes() no longer notes a
             * "was" for it (see there). This branch stays for a
             * session file written by the build before, so an undo
             * across the upgrade still honours what that load
             * recorded; a current load never reaches it. */
            if (sscanf(rec[i].note, "was b %d,%d", &omaj, &omin) == 2) {
                if (mknod(rec[i].what, S_IFBLK | 0660,
                          makedev(omaj, omin)) == 0) {
                    char det[64];

                    if (out != NULL)
                        fprintf(out, "#   %s put back (b %d %d)\n",
                                rec[i].what, omaj, omin);
                    sprintf(det, "b %d,%d - put back", omaj, omin);
                    vlhe_journal_add(VLHE_CH_NODE, rec[i].what, det);
                } else if (out != NULL) {
                    fprintf(out, "#   %s: could not put the earlier node"
                                 " back (b %d %d) - %s\n",
                            rec[i].what, omaj, omin, strerror(errno));
                }
            }

            (void) vlhe_session_mark(i, VLHE_SESS_UNDONE, NULL);
        } else {
            if (out != NULL)
                fprintf(out, "#   %s: cannot remove - %s\n",
                        rec[i].what, strerror(errno));
            vlhe_journal_failed(VLHE_CH_RMNODE, rec[i].what,
                                strerror(errno));
            (void) vlhe_session_mark(i, VLHE_SESS_FAILED,
                                     strerror(errno));
            failed++;
        }
        did++;
    }
    /* RECORDS ACTED ON, OR -1 IF ANY FAILED - design/47 section 5: the
     * caller read `> 0' as "covered" and a failed unlink never failed
     * the step. A failure is journalled above either way. */
    free(rec);
    return failed ? -1 : did;
}

static int
remove_nodes(int maj, int want_midi, FILE *out)
{
    int i, bad = 0;
    struct stat sb;
    char path[64];
    char det[64];

    for (i = 0; i < VLHE_MAX_DRIVE; i++) {
        sprintf(path, "/dev/vdisc%d", i);

        if (lstat(path, &sb) != 0 || !S_ISBLK(sb.st_mode))
            continue;
        /*
         * ONLY OURS. With maj <= 0 the config could not be read, so
         * we do not know which major was ours and remove nothing -
         * guessing would be how a user's own node disappears.
         */
        if (maj <= 0 || (int) major(sb.st_rdev) != maj)
            continue;

        sprintf(det, "was b %d,%d", (int) major(sb.st_rdev),
                (int) minor(sb.st_rdev));

        if (unlink(path) == 0) {
            if (out != NULL)
                fprintf(out, "#   %s removed\n", path);
            vlhe_journal_add(VLHE_CH_RMNODE, path, det);
        } else {
            if (out != NULL)
                fprintf(out, "#   %s: cannot remove - %s\n",
                        path, strerror(errno));
            vlhe_journal_failed(VLHE_CH_RMNODE, path, strerror(errno));
            bad = 1;
        }
    }

    /*
     * THE CONTROL NODE AND THE MIDI NODE, both character devices we
     * created. Major 10 is `misc' and 14 is OSS - checked, so a name
     * collision with something else's node does not cost the user
     * their device.
     */
    {
        static const struct { const char *path; int maj; } cnode[] = {
            { "/dev/vdiscctl", 10 },
            { "/dev/vmidi",    VMIDI_MAJOR }
        };
        int k;

        for (k = 0; k < 2; k++) {
            /*
             * EACH BELONGS TO ONE COMPONENT. /dev/vdiscctl goes with
             * the disc nodes (maj > 0 says this is the CD scope);
             * /dev/vmidi only when asked for. A CD-scoped unload that
             * took the MIDI node would strand a loaded vmidi with no
             * node - found on target 2026-09-23.
             */
            if (k == 0 && maj <= 0)
                continue;
            if (k == 1 && !want_midi)
                continue;
            if (lstat(cnode[k].path, &sb) != 0 || !S_ISCHR(sb.st_mode))
                continue;
            if ((int) major(sb.st_rdev) != cnode[k].maj)
                continue;

            sprintf(det, "was c %d,%d", (int) major(sb.st_rdev),
                    (int) minor(sb.st_rdev));

            if (unlink(cnode[k].path) == 0) {
                if (out != NULL)
                    fprintf(out, "#   %s removed\n", cnode[k].path);
                vlhe_journal_add(VLHE_CH_RMNODE, cnode[k].path, det);
            } else {
                if (out != NULL)
                    fprintf(out, "#   %s: cannot remove - %s\n",
                            cnode[k].path, strerror(errno));
                vlhe_journal_failed(VLHE_CH_RMNODE, cnode[k].path,
                                    strerror(errno));
                bad = 1;
            }
        }
    }

    return bad ? -1 : 0;
}

/*
 * `/dev/vmidi' - THE CHARACTER NODE vmidid OPENS.
 *
 * THE POLICY IS load.sh's, lines 765-815, restated in C rather than
 * invented a second time - and every clause of it is there because of
 * a failure on a real machine.
 *
 * THE MINOR MOVED FROM 15 TO 11 ON 2026-09-07, because 15 is
 * esssolo1's `/dev/dmfm'. Every machine that had run with MIDI before
 * then has a `/dev/vmidi' at 14,15 - and left alone, vmidid opens
 * THAT: on the Acer it is the Solo-1's FM device, whose fops has no
 * read, so vmidid got "read: Invalid argument" and exited.
 *
 * SO A NODE AT THE WRONG MINOR IS THE DANGEROUS CASE, not a missing
 * one. A missing node fails loudly at open; a wrong one SUCCEEDS
 * against another driver entirely and says nothing.
 *
 * ONLY A NODE NAMED vmidi AT THE WRONG MINOR IS TOUCHED. One at the
 * right minor is left exactly as found, whoever made it.
 *
 * AND THE unlink's RESULT IS CHECKED. load.sh's first version printed
 * "recreating", ignored a failed rm, then skipped the mknod because
 * the node still existed - and said nothing at all. That is the exact
 * sequence that left the Acer opening the wrong device.
 *
 * Returns 0, or -1 if the node could not be put right.
 */

/*
 * MAKE /dev/dspN FOR THE MINOR VSOUND GOT - design/38 section 9f2.
 *
 * WHY THIS IS NEEDED AT ALL. `MAKEDEV audio' creates exactly two dsp
 * nodes - /dev/dsp (minor 3) and /dev/dsp1 (19) - hardcoded,
 * whatever hardware is present. An es1371 registers TWO dsp minors
 * and takes both, so on a stock machine vsound gets minor 35 and
 * /dev/dsp2 DOES NOT EXIST. The module is loaded and unreachable:
 * nothing can open it, and no ioctl can be asked through a node that
 * is not there. /proc/vsound is how we learn the number; this is how
 * the node appears.
 *
 * IT NEVER REPLACES AN EXISTING NODE, AND THAT IS THE DIFFERENCE
 * FROM make_vmidi_node() BELOW. /dev/vmidi is ours by definition, so
 * a wrong one is recreated. A /dev/dspN may be A REAL CARD'S, made
 * by MAKEDEV or by the paid OSS, and removing it would take away the
 * user's sound device to solve a problem of our own. If the path
 * exists we leave it and say what we found.
 *
 * The minor for index N is 3 + 16*N - sound_core.c's chains[3] is
 * registered with low=3 and steps by 16.
 */
static int
make_dsp_node(int idx, FILE *out)
{
    char path[VLHE_PATH_MAX];
    struct stat sb;
    int minor_want;

    if (idx < 0)
        return 0;

    if (idx == 0)
        strcpy(path, "/dev/dsp");
    else
        sprintf(path, "/dev/dsp%d", idx);

    minor_want = 3 + idx * 16;

    if (lstat(path, &sb) == 0) {
        /*
         * ALREADY THERE. Say whether it is the right one - a node at
         * the wrong minor is a real problem the user has to fix, and
         * silence about it would leave them with a Volume page that
         * never fills and nothing to go on.
         */
        if (S_ISCHR(sb.st_mode)
            && (int) major(sb.st_rdev) == 14
            && (int) minor(sb.st_rdev) == minor_want)
            return 0;           /* right already */

        if (out != NULL)
            fprintf(out, "#   %s exists and is %d,%d - vsound is on"
                         " minor %d. NOT replaced: it may be a real"
                         " card's.\n"
                         "#   fix by hand if it is not:"
                         " rm %s; mknod %s c 14 %d\n",
                    path, (int) major(sb.st_rdev),
                    (int) minor(sb.st_rdev), minor_want,
                    path, path, minor_want);
        return -1;
    }

    if (mknod(path, S_IFCHR | 0666, makedev(14, minor_want)) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: mknod failed - %s. Nothing can"
                         " reach vsound without it\n",
                    path, strerror(errno));
        return -1;
    }

    /* Masked by umask, like the vmidi node below - said rather than
     * assumed. MAKEDEV gives the audio group; we cannot, not knowing
     * its gid, so the mode is what makes it usable. */
    if (chmod(path, 0666) != 0 && out != NULL)
        fprintf(out, "#   %s: chmod 666 failed - %s\n",
                path, strerror(errno));

    if (out != NULL)
        fprintf(out, "#   %s created (c 14 %d) - vsound\n",
                path, minor_want);
    return 0;
}

static int
make_vmidi_node(int want, FILE *out)
{
    struct stat sb;
    const char *path = "/dev/vmidi";

    if (want < 0)
        return 0;

    if (lstat(path, &sb) == 0) {
        if (S_ISCHR(sb.st_mode)
            && (int) major(sb.st_rdev) == VMIDI_MAJOR
            && (int) minor(sb.st_rdev) == want) {
            return 0;           /* right already - leave it alone */
        }

        if (out != NULL)
            fprintf(out, "#   %s is %d,%d - wanted %d,%d,"
                         " recreating\n",
                    path, (int) major(sb.st_rdev),
                    (int) minor(sb.st_rdev), VMIDI_MAJOR, want);

        if (unlink(path) != 0) {
            if (out != NULL)
                fprintf(out, "#   %s: cannot remove - %s."
                             "  vmidid would open the WRONG device;"
                             " fix by hand:\n"
                             "        rm %s; mknod %s c %d %d\n",
                        path, strerror(errno), path, path,
                        VMIDI_MAJOR, want);
            return -1;
        }
    }

    {
        char rdet[64];

        /* RECORDED BEFORE IT IS MADE - design/54 7h Stage 1.5. */
        sprintf(rdet, "c %d,%d", VMIDI_MAJOR, want);
        if (node_record_first(path, rdet, out) != 0)
            return -1;
    }
    if (mknod(path, S_IFCHR | 0666, makedev(VMIDI_MAJOR, want)) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: mknod failed - %s."
                         "  vmidid and vmidicat will not start\n",
                    path, strerror(errno));
        node_record_unmade(path);
        return -1;
    }

    /* mknod's mode is masked by umask, so 0666 above may have landed
     * as 0644 - which a non-root client cannot write. Said plainly
     * rather than assumed, as load.sh's chmod does. */
    if (chmod(path, 0666) != 0 && out != NULL)
        fprintf(out, "#   %s: chmod 666 failed - %s\n",
                path, strerror(errno));

    if (out != NULL)
        fprintf(out, "#   %s created (c %d %d)\n",
                path, VMIDI_MAJOR, want);

    {
        char det[64];

        sprintf(det, "c %d,%d", VMIDI_MAJOR, want);
        vlhe_journal_add(VLHE_CH_NODE, path, det);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stopping a daemon                                                  */
/* ------------------------------------------------------------------ */

/*
 * IS THIS PID REALLY THE DAEMON WE MEAN?
 *
 * A PID FILE IS NOT PROOF. It says what a process's id WAS when it
 * started; by the time we read it that process may be gone and the
 * number reused by something else entirely - the user's editor, their
 * X session. Signalling on the strength of a stale file is how a
 * teardown kills a bystander.
 *
 * `/proc/<pid>/stat' holds the command name in parentheses as its
 * second field, and 2.2 has it. Comparing that to the name we expect
 * costs one small read and turns "this file says 632" into "pid 632
 * is running vsoundd".
 *
 * NOT PERFECT AND BETTER THAN NOTHING: another copy of the same
 * daemon would pass, which is fine - it IS one of ours. What it stops
 * is the dangerous case, an unrelated program inheriting the number.
 *
 * FAILS CLOSED. If /proc cannot be read, or the name does not match,
 * this returns 0 and nothing is signalled.
 */
/*
 * BY THE PROGRAM FILE FIRST - design/54 D61, 2026-10-04. `comm' holds
 * only 15 characters (2.2's task_struct comm is 16 with the NUL), so a
 * name longer than that could never match it, and two names sharing
 * their first 15 would match each other. /proc/<pid>/exe names the file
 * the process runs, in full. 2.2 appends " (deleted)" when that file
 * has been replaced - which a package upgrade does to a running daemon
 * - and that suffix is not part of the name, or an Unload after an
 * upgrade could not stop its own daemons. Root can read any process's
 * link; when it cannot be read, the stat name below is the answer, held
 * to its 15 characters.
 */
#define PID_COMM_MAX 15

static int
pid_is_named(int pid, const char *name)
{
    char  path[64];
    char  buf[256];
    FILE *fp;
    char *open_paren, *close_paren;
    size_t n;
    int   len;

    if (pid <= 0 || name == NULL)
        return 0;

    sprintf(path, "/proc/%d/exe", pid);
    len = (int) readlink(path, buf, sizeof buf - 1);
    if (len > 0) {
        const char *b;
        static const char deleted[] = " (deleted)";
        int dl = (int) sizeof deleted - 1;

        buf[len] = '\0';
        if (len > dl && strcmp(buf + len - dl, deleted) == 0)
            buf[len - dl] = '\0';
        b = strrchr(buf, '/');
        b = (b != NULL) ? b + 1 : buf;
        return strcmp(b, name) == 0;
    }

    sprintf(path, "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (fp == NULL)
        return 0;

    n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    if (n == 0)
        return 0;
    buf[n] = '\0';

    /* `1234 (vsoundd) S ...' - the name is BRACKETED, and it may
     * itself contain a space, so take the LAST ')' rather than the
     * first. */
    open_paren  = strchr(buf, '(');
    close_paren = strrchr(buf, ')');
    if (open_paren == NULL || close_paren == NULL || close_paren <= open_paren)
        return 0;

    *close_paren = '\0';
    if ((int) strlen(name) > PID_COMM_MAX)
        return strncmp(open_paren + 1, name, PID_COMM_MAX) == 0;
    return strcmp(open_paren + 1, name) == 0;
}

/*
 * STOP ONE DAEMON, and this is the most dangerous function in the
 * file. CLAUDE.md section 1 records kill(-1, SIGKILL) taking the
 * user's X session TWICE on 2026-09-14, from a pid field that had
 * been reset to -1. Everything below is shaped by that.
 *
 *   1. the pid comes from the file, and 0 or negative is refused -
 *      0 means the caller's whole process group and -1 means every
 *      process the user owns
 *   2. /proc must agree the pid is running a process of that NAME
 *   3. SIGTERM ONLY. No SIGKILL anywhere in this function. A daemon
 *      that ignores SIGTERM is reported as still running and left
 *      alone, because the alternative is guessing about a process we
 *      have already failed to identify confidently
 *   4. the wait is bounded, and a timeout is a report rather than an
 *      escalation
 *
 * Returns 0 if it is gone (or was never there), 1 if it is still up.
 */
/* THE LAST RESORT, defined below run_command() which it uses.
 * See its own comment for why it is a last resort and not a
 * first one. */
static int killall_stray(const char *name, FILE *out);
static void remember_child(pid_t pid);

static int
stop_daemon(const char *name, FILE *out)
{
    const char *pf;
    int         pid, i;

    pf = vlhe_status_pidfile_path(name, -1);
    if (pf == NULL)
        return 0;

    pid = vlhe_status_read_pidfile(pf);

    /*
     * NO PIDFILE, OR A STALE ONE - so there is no pid to signal. That
     * is the NORMAL case on a machine where nothing was running, and
     * it is also exactly the case where a STRAY can be hiding: the
     * daemon is up and the file that should name it is gone or names
     * a corpse.
     *
     * SO ASK BY NAME BEFORE CONCLUDING THERE IS NOTHING TO DO. On a
     * clean machine killall finds nothing and this costs one process;
     * when it finds something, that is the finding.
     */
    if (pid <= 0 || !vlhe_status_pid_alive(pid)) {
        /* A PID FILE WITH NO PROCESS BEHIND IT GOES - 2026-10-01. The
         * daemon died or refused to start and could not remove it;
         * left here, the Status page reported "stale pid file - it
         * died" for the rest of the session about a daemon this very
         * unload had been asked to stop. Only when the pid is dead:
         * a live pid is signalled below and removes its own file. */
        if (pid > 0 && unlink(pf) == 0 && out != NULL)
            fprintf(out, "#   removed a stale pid file (%d is gone)\n",
                    pid);
        killall_stray(name, out);
        return 0;
    }

    if (!pid_is_named(pid, name)) {
        if (out != NULL)
            fprintf(out, "#   pid %d is not %s - NOT signalled\n",
                    pid, name);
        return 1;
    }

    if (out != NULL) {
        fprintf(out, "#   SIGTERM %d\n", pid);
        fflush(out);
    }

    /* GUARDED AGAIN, ON THE LINE BEFORE THE CALL. pid was checked
     * above and nothing since could have changed it - the guard is
     * here because CLAUDE.md asks for it on every kill(), however
     * obvious, and an obvious one is exactly what went wrong before. */
    if (pid > 0)
        kill(pid, SIGTERM);

    /*
     * UP TO FIVE SECONDS. vsoundd closes its device and writes a
     * summary line on the way out; vmidid does the same. A second was
     * not enough on the Acer's disk.
     *
     * AND THE waitpid() IS THE FIX FOR A BUG THIS LOOP HAD ALL ALONG,
     * measured on 86Box 2026-09-23 (tests/logs/2026-09-23-gui-load-
     * unload/). The journal said
     *
     *     stop   daemon vdiscd  *** NOT DONE: it did not stop
     *     stop   daemon vsoundd  *** NOT DONE: it did not stop
     *     stop   daemon vmidid  *** NOT DONE: it did not stop
     *
     * for all three daemons, on both unload runs - while DAEMON.LOG
     * showed every one of them shutting down cleanly in the same
     * window, vmidid printing its full three-line closing summary,
     * and every rmmod afterwards SUCCEEDING, which is impossible if
     * anything still held the devices.
     *
     * THE CAUSE IS A ZOMBIE. run_command() forks the daemons and its
     * own comment says "none of our daemons daemonises: no fork, no
     * setsid, they run in the foreground" - so the forked child IS
     * the daemon and stays OUR CHILD. setsid() detaches the session,
     * not the parent. Nothing reaps it: run_command() does one
     * waitpid(WNOHANG) at start-up to catch an immediate failure and
     * never looks again, and there is no SIGCHLD handler anywhere in
     * the program.
     *
     * A ZOMBIE ANSWERS kill(pid, 0) WITH SUCCESS. It has exited, its
     * exit status is waiting to be collected, and the pid is still
     * allocated - so vlhe_status_pid_alive() said "alive" for all
     * fifty iterations of a daemon that had already written its
     * goodbye.
     *
     * SO THE LOOP REAPS FIRST. waitpid(WNOHANG) collects a child that
     * has exited, the pid is then genuinely gone, and the next
     * kill(pid, 0) answers correctly.
     *
     * A PID THAT IS NOT OUR CHILD - started by the init script, or by
     * a previous run of the GUI - returns -1 from waitpid with ECHILD
     * and falls through to exactly the check this loop always did.
     * That case is unaffected.
     */
    for (i = 0; i < 50; i++) {
        int   status;
        pid_t got = waitpid(pid, &status, WNOHANG);

        if (got == pid) {
            /*
             * SAID OUT LOUD, AND DELIBERATELY, so the next target run
             * can tell WHICH path stopped the daemon rather than only
             * that it stopped. The user asked for this: "Add extra
             * logging or something to ensure we can catch if it was
             * changed or not".
             *
             * This line appearing means the fix did the work - the
             * daemon was a zombie and we collected it. Its ABSENCE on
             * a successful stop means the pid went away by another
             * route and the zombie theory does not explain that run.
             */
            if (out != NULL)
                fprintf(out, "#   reaped %d after %d ms"
                             " (was a zombie)\n", pid, i * 100);
            return 0;
        }

        if (!vlhe_status_pid_alive(pid)) {
            /* NOT OUR CHILD, or already reaped elsewhere - the pid is
             * gone and that is a clean stop either way. Named
             * differently from the line above so the two cases are
             * distinguishable in a capture. */
            if (out != NULL)
                fprintf(out, "#   %d gone after %d ms\n",
                        pid, i * 100);
            return 0;
        }
        usleep(100000);
    }

    /*
     * STILL THERE AFTER FIVE SECONDS. Report what it IS, because
     * "did not stop" was the misleading half of the bug above and a
     * reader deserves to know whether the process is running or
     * merely uncollected.
     */
    {
        int   status;
        pid_t got = waitpid(pid, &status, WNOHANG);

        /* THE REAP HAPPENS WHETHER OR NOT ANYONE IS LISTENING. An
         * earlier draft of this block did it inside `if (out !=
         * NULL)', which would have made a silent caller behave
         * differently from a reporting one - the shape of bug this
         * whole function is being fixed for. */
        if (got == pid) {
            if (out != NULL)
                fprintf(out, "#   %d exited on the last check"
                             " - collected\n", pid);
            return 0;
        }
        /*
         * IT WOULD NOT STOP. The pidfile path has done everything it
         * can - SIGTERM, five seconds, a reap - so this is where the
         * last resort belongs, AFTER the precise path has failed and
         * said so rather than instead of it.
         */
        if (out != NULL)
            fprintf(out, "#   still running after 5s\n");

        if (killall_stray(name, out))
            return 0;           /* gone now, and the journal says how */

        if (out != NULL)
            fprintf(out, "#   and killall did not reach it either"
                         " - left alone\n");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Running a plan                                                     */
/* ------------------------------------------------------------------ */

/*
 * IS THIS STEP A NOTE RATHER THAN A COMMAND?
 *
 * plan_unload() and the CHECK step write things like
 *
 *     (stop vdiscd)
 *     (sound.o is not removed)
 *
 * in parentheses, precisely so a dry run reads as prose where there
 * is nothing to run. THE PARENTHESIS IS THE MARKER, and treating it
 * as one keeps the executor from ever handing `(stop vdiscd)' to a
 * shell - which would fail confusingly rather than harmlessly.
 */
static int
step_is_note(const char *cmd)
{
    return cmd != NULL && cmd[0] == '(';
}

/*
 * RUN ONE COMMAND AND WAIT.
 *
 * fork + execvp RATHER THAN system(), for two reasons that both
 * matter here:
 *
 *   1. NO SHELL. system() hands the string to /bin/sh, so a font
 *      path with a space or a quote in it becomes several arguments
 *      or an error. vmidid's -s takes a path the user chose.
 *   2. THE EXIT STATUS IS THE ANSWER. system() folds "the shell
 *      could not run it" and "it ran and failed" into one number.
 *
 * SPLITTING ON SPACES IS ENOUGH AND IS NOT A SHORTCUT. Every command
 * in a plan is built by insmod_cmd() or a sprintf in this file from
 * values the config validated - there is no user-supplied string
 * reaching argv un-split except a font path, which is the one case
 * the caller must keep space-free. Recorded rather than left
 * implicit, because a future step that interpolates a filename would
 * break this assumption silently.
 *
 * Returns the exit status, or -1 if the child could not be run.
 */
/*
 * @VSOUND@ AND @CARD@ - RESOLVED WHEN THE STEP RUNS, NOT WHEN THE
 * PLAN IS BUILT.
 *
 * WHY A PLACEHOLDER AT ALL. The plan is a list of strings built
 * before any of it executes, and the daemons need two numbers that
 * CANNOT BE KNOWN THEN: which node vsound took, and which the card
 * came back on. design/38 section 7b has the case that proved
 * predicting them wrong - es1371 on dsp0+dsp1 with the pick on dsp1,
 * where `rmmod' frees BOTH, vsound's first-free walk lands on dsp0
 * rather than the pick, and the card returns SPLIT across dsp1 and
 * dsp3. The arithmetic and the machine disagree, and the machine is
 * right.
 *
 * SO THE PLAN SAYS WHAT IT WANTS AND THE RUNNER ASKS. By the time a
 * daemon step runs, `insmod vsound.o' and the card's `modprobe' are
 * behind it, so both answers exist to be read.
 *
 * AND THE PRINTED PLAN IS HONEST. `vlhe_plan_print()' shows
 * `-v @VSOUND@', which reads as "whatever vsound turns out to be" -
 * where a predicted number would be a claim that might not survive
 * contact with the machine.
 *
 * THE JOURNAL RECORDS THE EXPANDED FORM, because journal_step() runs
 * after the command has succeeded and logs what the plan held - see
 * its own note. That is the one place a reader wants the number
 * rather than the intent.
 */
static void
probe_vsound(char *out, size_t max)
{
    int i;
    int idx;

    /*
     * `/proc/vsound' FIRST, AND IT IS THE ANSWER - the module
     * publishes the minor `register_sound_dsp()' gave it, so this is
     * vsound saying where it is rather than us inferring it.
     *
     * THE SWEEP BELOW GETS THIS WRONG THE MOMENT `ProgramsUse' IS NOT
     * `/dev/dsp', and it broke a machine on 2026-09-25 (design/36
     * rows 77 and 78). It walks the nodes in order and stops at the
     * first that answers VSOUND_IOC_STAT - and the REDIRECT answers,
     * because it is vsound under another name. With
     * `ProgramsUse = /dev/dsp2' every daemon was handed `/dev/dsp2'
     * while vsound was on `/dev/dsp3', so a game configured for that
     * same node met the daemons there instead of getting its own
     * channel, and was refused.
     *
     * IT HID FOR ELEVEN HOURS BECAUSE THE TWO AGREE ON THE DEFAULT.
     * This function was written at 01:22 (`5c28d0b') and
     * `/proc/vsound' arrived at 12:23 (`f976116') for exactly this
     * job - design/38 section 9f2 calls it "the case that defeats
     * probing" - and nothing came back to use it here. On
     * `ProgramsUse = /dev/dsp' the alias and the sweep's first
     * candidate are the same path, so the right answer and the wrong
     * one coincide.
     *
     * The `(dspnode)' step already asks `vlhe_vsound_dsp()'. Two
     * sources of truth for one fact is the defect; this makes it one.
     */
    idx = vlhe_vsound_dsp();
    if (idx == 0) {
        strncpy(out, "/dev/dsp", max - 1);
        out[max - 1] = '\0';
        return;
    }
    if (idx > 0) {
        sprintf(out, "/dev/dsp%d", idx);
        return;
    }

    /*
     * NO `/proc/vsound' - an older module, or one that failed to
     * create the entry. Sweep as before, and SKIP SYMLINKS: on this
     * path the redirect is exactly what we must not return, and
     * `lstat' is how we tell our own alias from a real node.
     */
    for (i = 0; i < VLHE_DSP_PROBE_MAX; i++) {
        char node[VLHE_PATH_MAX];
        struct vsound_stat st;
        struct stat sb;
        int fd, rc;

        if (i == 0)
            strcpy(node, "/dev/dsp");
        else
            sprintf(node, "/dev/dsp%d", i);

        if (lstat(node, &sb) != 0 || !S_ISCHR(sb.st_mode))
            continue;           /* absent, or our own redirect */
        /* LOOKING MUST NOT LOAD A MODULE - design/55 R11, design/54 D53 (2026-10-04): an empty
         * minor with an alias for it loads a driver on open. */
        if ((int) major(sb.st_rdev) == 14
            && vlhe_modconf_open_would_load((int) minor(sb.st_rdev), NULL, 0))
            continue;

        fd = open(node, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        rc = ioctl(fd, VSOUND_IOC_STAT, &st);
        close(fd);
        if (rc == 0) {
            strncpy(out, node, max - 1);
            out[max - 1] = '\0';
            return;
        }
    }
    /* NOT FOUND - answer /dev/dsp, so a daemon started on a machine
     * where vsound failed to load fails the way it always did rather
     * than being handed an empty string. */
    strncpy(out, "/dev/dsp", max - 1);
    out[max - 1] = '\0';
}

/*
 * THE CARD'S NODE: THE USER'S PICK, OR THE LOWEST ONE THAT IS NOT
 * VSOUND.
 *
 * THE PICK COMES FIRST, AND UNDER THE SYMLINK DESIGN IT IS SIMPLY
 * RIGHT. `CardDevice' is the node the user's speakers are on
 * (design/38 section 9), and nothing moves it: the card is never
 * unloaded, never reloaded, and keeps the minor it booted with. So
 * the pick names a live node at plan time and still names it when
 * the pump starts.
 *
 * THAT IS THE WHOLE DIFFERENCE FROM THE DISPLACEMENT DESIGN, where
 * the card WAS cycled and its node afterwards had to be discovered.
 * The probe below is what answered that, and it survived the
 * redesign as the only answer rather than as the fallback it should
 * have been - so a user who picked `sb' on a two-card machine got
 * whichever node sorted lowest instead.
 *
 * EMPTY MEANS DETECT, which is what `CardDevice' has meant since
 * 2026-09-24, so the probe stays for that case and for a pick that
 * names something absent.
 *
 * THE PROBE, THEN: THE LOWEST NODE THAT IS NOT VSOUND.
 *
 * PROVABLE RATHER THAN OBSERVED, for a two-DSP card. `dev_audio' is
 * registered before `dev_dac' in every such driver here and
 * __sound_insert_unit() walks UPWARD, so `dev_dac' always lands above
 * `dev_audio' and the lowest non-vsound node is the primary, duplex
 * device.
 *
 * AND ON ONE CARD THAT CHOICE IS AUDIBLE. The es1371's two DACs are
 * equivalent (both 4000-48000 through the SRC), but the es1370's
 * second is a fixed-clock divider - 5512/11025/22050/44100 only,
 * ceiling 44100, wired to the mixer's SYNTH control. Pumping to the
 * wrong one there costs 48 kHz and moves the level to another
 * slider. design/38 section 7b.
 *
 * WHICH IS A SECOND REASON TO PREFER THE PICK: on that card the
 * probe's answer is a guess about which DAC the user means, and the
 * pick is the user saying.
 */
static void
probe_card(const char *vsound_node, char *out, size_t max)
{
    int i;
    struct vlhe_sound snd;

    /*
     * THE PICK, IF THERE IS ONE AND IT IS USABLE.
     *
     * TWO CHECKS, AND THE SECOND IS THE ONE THAT MATTERS. A node
     * that does not exist would give the pump a path it cannot
     * open; a node that IS vsound would give it the self-loop this
     * design exists to prevent - `pumping /dev/dsp -> /dev/dsp',
     * which a real plan produced on 2026-09-24.
     *
     * NEITHER IS EXPECTED. The GUI lists only nodes that answered a
     * probe, so a pick naming something absent means the machine
     * changed under the config. Falling through to the probe is the
     * right answer to that rather than failing: it is what an empty
     * pick gets, and the user gets sound.
     */
    if (vlhe_sound(&snd) == 0 && snd.card[0] != '\0') {
        struct stat sb;
        char        renamed[VLHE_PATH_MAX];

        renamed[0] = '\0';

        /*
         * THE PICK MAY BE THE PATH WE JUST REDIRECTED - row 77's
         * second half, found on target 2026-09-25.
         *
         * With both settings on `/dev/dsp2' the run went:
         * `create node /dev/dsp2 -> /dev/dsp3', then the pump
         * `pumping /dev/dsp3 -> /dev/dsp0'. `lstat' saw our own
         * symlink, `S_ISCHR' failed, the pick was discarded and the
         * sweep sent the audio to the FIRST card at minor 3 instead
         * of the one the user chose.
         *
         * FOLLOWING THE LINK IS THE WRONG FIX AND I WROTE IT FIRST.
         * `/dev/dsp2' now resolves to VSOUND, so a `stat' that
         * follows it would hand the pump the mixer and produce
         * silence - the self-loop this design exists to prevent.
         *
         * THE ANSWER IS THE NAME `name_the_card()' GAVE THE CARD,
         * which the load wrote into the session note as
         * `card at /dev/dspN'. The load knows it and nothing else
         * can work it out, so it is carried rather than re-derived.
         */
        {
            int n;
            struct vlhe_sess_rec *rec = vlhe_session_read_all(&n);
            int k;

            for (k = n - 1; k >= 0; k--) {
                char *at;

                if (rec[k].kind != VLHE_SESS_LINK
                    || !rec_pending(&rec[k])
                    || strcmp(rec[k].what, snd.card) != 0)
                    continue;
                at = strstr(rec[k].note, "card at ");
                if (at != NULL) {
                    strncpy(renamed, at + 8, sizeof renamed - 1);
                    renamed[sizeof renamed - 1] = '\0';
                }
                break;
            }
            free(rec);
        }

        if (renamed[0] != '\0'
            && lstat(renamed, &sb) == 0 && S_ISCHR(sb.st_mode)) {
            strncpy(out, renamed, max - 1);
            out[max - 1] = '\0';
            return;
        }

        /* NOT REDIRECTED - the ordinary case, where the pick still
         * names a real node and is simply right. */
        if (!(vsound_node != NULL && strcmp(snd.card, vsound_node) == 0)
            && lstat(snd.card, &sb) == 0 && S_ISCHR(sb.st_mode)) {
            strncpy(out, snd.card, max - 1);
            out[max - 1] = '\0';
            return;
        }
    }

    /*
     * NEVER /dev/dsp ITSELF - it is a SYMLINK to vsound by the time
     * the daemons start (design/38 section 9), so opening it would
     * hand the pump its own device and produce the self-loop this
     * whole design exists to prevent. The numbered nodes are the
     * real ones; /dev/dsp0 is the card's own name when the card had
     * been on minor 3, made by name_the_card().
     *
     * AND IT DOES NOT OPEN A CANDIDATE TO TEST IT. `esssolo1's open
     * BLOCKS rather than returning EBUSY (CLAUDE.md section 5), so
     * probing a busy card that way can hang the runner. lstat() says
     * whether a node exists, and VSOUND_IOC_STAT says whether it is
     * ours - and that ioctl is only attempted on a node we are about
     * to reject, never on one we mean to use.
     */
    for (i = 0; i < VLHE_DSP_PROBE_MAX; i++) {
        char node[VLHE_PATH_MAX];
        struct stat sb;

        sprintf(node, "/dev/dsp%d", i);

        if (vsound_node != NULL && strcmp(node, vsound_node) == 0)
            continue;           /* ours, not a card */

        if (lstat(node, &sb) != 0 || !S_ISCHR(sb.st_mode))
            continue;           /* no such node */
        /* NOR ONE WHOSE OPEN WOULD LOAD A MODULE - design/55 R11, design/54 D53 (2026-10-04). The
         * pump opens what this picks, as root, and an aliased empty
         * minor would load that driver on every try. */
        if ((int) major(sb.st_rdev) == 14
            && vlhe_modconf_open_would_load((int) minor(sb.st_rdev), NULL, 0))
            continue;

        /* THE LOWEST ONE THAT IS NOT OURS - design/38 7b: dev_audio
         * registers before dev_dac and the allocator walks upward,
         * so the lowest is the primary duplex device. On an es1370
         * that choice is audible. */
        strncpy(out, node, max - 1);
        out[max - 1] = '\0';
        return;
    }
    out[0] = '\0';              /* no card - the caller omits the flag */
}

/*
 * DOES THIS COMMAND RESOLVE `@VSOUND@' FOR ITSELF? - design/43 part A.
 *
 * `vmidid' and `vdiscd' call vlhe_resolve_out() on every open, so
 * they must receive the TOKEN and not a path: expanding it here
 * would freeze the answer at plan-run time, which is the whole
 * defect part A exists to fix - a daemon started before vsound
 * would hold a stale name for its entire life.
 *
 * EVERY OTHER STEP STILL GETS THE EXPANSION. `vsoundd -v @VSOUND@'
 * is the clearest case: the pump is started in the same plan that
 * loaded vsound, it opens once, and it has no retry loop - so a
 * resolved path is exactly right for it.
 *
 * MATCHED ON THE PROGRAM NAME, after any directory. The plan writes
 * an absolute path in portable mode and a bare name when installed,
 * so the basename is the only stable part.
 */
static int
cmd_resolves_own_out(const char *cmd)
{
    const char *base;
    const char *sp;
    char        name[64];
    size_t      n;

    if (cmd == NULL)
        return 0;
    sp = strchr(cmd, ' ');
    n  = (sp != NULL) ? (size_t) (sp - cmd) : strlen(cmd);
    if (n == 0 || n >= sizeof name)
        return 0;
    memcpy(name, cmd, n);
    name[n] = '\0';

    base = strrchr(name, '/');
    base = (base != NULL) ? base + 1 : name;

    return strcmp(base, "vmidid") == 0 || strcmp(base, "vdiscd") == 0;
}

/* Replace every @VSOUND@ and @CARD@ in `buf'. Probed ONCE per call,
 * and only when a placeholder is actually present - most steps have
 * none and pay nothing.
 *
 * `keep_vsound' leaves `@VSOUND@' in place for the daemons that
 * resolve it themselves - see cmd_resolves_own_out() above. @CARD@
 * and @FONTn@ are expanded either way: neither is re-resolvable at
 * run time and both name things that do not move. */
/* The byte expand_nodes() uses for a space inside a substituted value,
 * undone by run_command() after the split. 0x01 is in no path. */
#define ARG_SPACE '\001'

/* The shell renderers - defined with the simulation below, used by
 * the runner and its cleanup above them. */
static void shell_heading(FILE *fp, int comp, int *last);
static void shell_dspnode(FILE *fp, int idx, const char *use);
static void shell_vmidinode(FILE *fp, int minor);
static void shell_vdisc_nodes(FILE *fp, int major, int n);
static void shell_rmnodes_cd(FILE *fp, int n);
static void shell_rmnodes_midi(FILE *fp);
static void shell_cdromlink(FILE *fp);
static void shell_cdromrestore(FILE *fp, const char *saved);
static void shell_restore(FILE *fp, const char *path, const char *prior);
static void shell_stop(FILE *fp, const char *name);
static void shell_expanded(FILE *fp, const char *buf, int daemon);

static void
expand_nodes(char *buf, size_t max, int keep_vsound)
{
    char vs[VLHE_PATH_MAX];
    char card[VLHE_PATH_MAX];
    int  did_probe = 0;

    /*
     * FONTS ARE FETCHED ONCE, LIKE THE PROBE - design/36 row 91.
     * `vlhe_fonts()' reads the config, and a command with two tags
     * would otherwise read it twice for the same answer.
     */
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    int   nfonts = -1;

    for (;;) {
        char *at = keep_vsound ? NULL : strstr(buf, "@VSOUND@");
        char *ac = strstr(buf, "@CARD@");
        char *af = strstr(buf, "@FONT");
        /* `@DRIVES@' - vdiscd's drive state file (2026-10-03). A tag
         * rather than the path written into the step, because a
         * portable folder may have a space in its name, and only a
         * SUBSTITUTED value is protected from the split - M5 below. */
        char *ad = strstr(buf, "@DRIVES@");
        char *where;
        const char *with;
        size_t taglen;
        char  tail[VLHE_CMD_MAX];
        int   fidx = -1;

        /* `@FONTn@' ONLY - a bare `@FONT' with no digit and no
         * closing `@' is not ours and must be left alone rather than
         * consumed as a malformed tag. */
        if (af != NULL
            && !(af[5] >= '0' && af[5] <= '9' && af[6] == '@'))
            af = NULL;

        if (at == NULL && ac == NULL && af == NULL && ad == NULL)
            return;

        /* THE PROBE ONLY FOR THE TAGS THAT NEED IT - @DRIVES@ and
         * @FONTn@ are files, and vdiscd's line keeps @VSOUND@. */
        if (!did_probe && (at != NULL || ac != NULL)) {
            probe_vsound(vs, sizeof vs);
            probe_card(vs, card, sizeof card);
            did_probe = 1;
        }

        /* LEFTMOST FIRST, so a command with all three resolves in
         * one pass per tag rather than depending on the order the
         * tests happen to be written in. */
        where = NULL; with = ""; taglen = 0;
        if (at != NULL) {
            where = at; with = vs; taglen = 8;          /* @VSOUND@ */
        }
        if (ac != NULL && (where == NULL || ac < where)) {
            where = ac; with = card; taglen = 6;        /* @CARD@   */
        }
        if (af != NULL && (where == NULL || af < where)) {
            where = af; taglen = 7;                     /* @FONTn@  */
            fidx = af[5] - '0';
        }
        if (ad != NULL && (where == NULL || ad < where)) {
            where = ad; taglen = 8;                     /* @DRIVES@ */
            with = vlhe_conf_drives_path();
            fidx = -1;
        }

        if (fidx >= 0) {
            if (nfonts < 0)
                nfonts = vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL);
            /* AN UNRESOLVABLE TAG IS LEFT IN PLACE, not blanked. The
             * daemon then fails with a visible `no such file' naming
             * `@FONT0@' rather than starting with one font missing
             * and sounding wrong for a reason nothing reports. */
            if (fidx >= nfonts || fonts[fidx].path[0] == '\0')
                return;
            with = fonts[fidx].path;
        }

        strncpy(tail, where + taglen, sizeof tail - 1);
        tail[sizeof tail - 1] = '\0';
        if ((where - buf) + strlen(with) + strlen(tail) >= max) {
            /* WOULD NOT FIT - LEFT IN PLACE, AND SAID - design/47 A2.
             * The command then runs with the literal tag and fails on
             * it, which is visible; this used to leave no account of
             * WHY. stderr is the CLI's terminal and the GUI's launch
             * terminal; nothing closer to the user exists here. */
            fprintf(stderr, "vlhe: command too long to expand %.*s -"
                            " left as it is (limit %lu bytes)\n",
                    (int) taglen, where, (unsigned long) max);
            return;
        }
        strcpy(where, with);
        /*
         * A SPACE IN THE VALUE MUST SURVIVE THE SPLIT - design/47 M5,
         * 2026-09-30. run_command() breaks the line into argv on
         * spaces, so a soundfont under a directory with one in its
         * name - Corel's own have them - arrived as several
         * arguments and the synth failed to open a path that did not
         * exist. The comment here said the caller must keep paths
         * space-free; nothing did. So a space inside a SUBSTITUTED
         * value is marked with a byte no path carries, and
         * run_command() puts the spaces back in each argument after
         * splitting. The command's own spaces are untouched.
         */
        {
            char *q;
            for (q = where; q < where + strlen(with); q++)
                if (*q == ' ')
                    *q = ARG_SPACE;
        }
        strcat(where, tail);
    }
}

/*
 * THE LINE INTO argv, on spaces, at most `max' words, NULL-terminated.
 * Then the spaces inside a substituted path come back - M5, see
 * expand_nodes(): each argument is its own string now, so this cannot
 * split anything. Returns the count.
 */
static int
split_args(char *buf, char **argv, int max)
{
    int argc = 0, k;
    char *p;

    for (p = strtok(buf, " "); p != NULL && argc < max; p = strtok(NULL, " "))
        argv[argc++] = p;
    argv[argc] = NULL;

    for (k = 0; k < argc; k++)
        for (p = argv[k]; *p != '\0'; p++)
            if (*p == ARG_SPACE)
                *p = ' ';
    return argc;
}

/* FOR THE HOST TESTS: expand and split as run_command() would, without
 * running anything. `buf' receives the expanded line and `argv' points
 * into it. Returns the count. */
int
vlhe_apply_expand_split(const char *cmd, char *buf, size_t max,
                        char **argv, int argmax)
{
    strncpy(buf, cmd, max - 1);
    buf[max - 1] = '\0';
    expand_nodes(buf, max, cmd_resolves_own_out(cmd));
    return split_args(buf, argv, argmax);
}

/* ------------------------------------------------------------------ *
 * The daemons' account - design/33 section 3k
 * ------------------------------------------------------------------ *
 *
 * THE RULING, THE USER 2026-09-30: an installed VLHE runs its daemons
 * as a dedicated `vlhe' account, never root; a portable copy leaves
 * the system as it found it and runs them as whoever pressed Load.
 * What it buys: vdiscd opens an image with an ORDINARY user's rights,
 * so `/root/private.iso' is refused by the kernel rather than served
 * as a CD to anyone (design/49 finding 4, and the confused deputy in
 * section 3k), and a daemon that misbehaves can damage nothing root
 * owns.
 *
 * THE DROP IS IN THE CHILD, between fork() and execvp() - no `su',
 * which would need a shell for an account whose shell is /bin/false.
 * Before it, still root, the child hands the account what it must
 * write: the run directory (pid files, the control FIFOs) and the
 * drive state's directory. Then the account's groups, its gid, its
 * uid, in that order - setuid() last, because after it the other two
 * are no longer allowed - and a check that it took, or the child
 * exits 126 rather than run as root by accident.
 */

/* OUR DAEMONS, by program name - one list for every check. */
static const char *const g_daemon_names[] = {
    "vsoundd", "vmidid", "vdiscd", NULL
};

int
vlhe_apply_is_our_daemon(const char *argv0)
{
    const char *b;

    if (argv0 == NULL)
        return 0;
    b = strrchr(argv0, '/');
    b = (b != NULL) ? b + 1 : argv0;
    {
        int i;

        for (i = 0; g_daemon_names[i] != NULL; i++)
            if (strcmp(b, g_daemon_names[i]) == 0)
                return 1;
    }
    return 0;
}

/* THE LIST, for the host test that holds every name to 15 characters
 * and none sharing its first 15 with another - design/54 D61. */
const char *const *
vlhe_apply_daemon_names(void)
{
    return g_daemon_names;
}

int
vlhe_apply_step_satisfied(const struct vlhe_step *st, char *why, int max)
{
    char name[64];

    if (st == NULL)
        return 0;
    if (why != NULL && max > 0)
        why[0] = '\0';

    /* A MODULE LOAD WHOSE MODULE IS IN. step_module() names insmod and
     * modprobe steps only, so an rmmod - where the module being loaded
     * is the normal case - can never match. */
    step_module(st, name, sizeof name);
    if (name[0] != '\0') {
        if (!vlhe_status_module_loaded(name))
            return 0;
        if (why != NULL && max > 0) {
            strncpy(why, "already loaded", (size_t) max - 1);
            why[max - 1] = '\0';
        }
        return 1;
    }

    /* ONE OF OUR DAEMONS, ALREADY RUNNING. Its first word is the
     * binary; "(stop vmidid)" and the other pseudo-steps start with a
     * parenthesis and are never this. One vdiscd serves every drive,
     * so each daemon has one pid file (the stop step reads the same). */
    if (st->kind == VLHE_STEP_DAEMON && st->cmd[0] != '(') {
        const char *b, *e;
        size_t n;

        e = strchr(st->cmd, ' ');
        n = e != NULL ? (size_t)(e - st->cmd) : strlen(st->cmd);
        if (n >= sizeof name)
            return 0;
        memcpy(name, st->cmd, n);
        name[n] = '\0';
        if (!vlhe_apply_is_our_daemon(name))
            return 0;
        b = strrchr(name, '/');
        b = (b != NULL) ? b + 1 : name;
        if (vlhe_status_daemon(vlhe_status_pidfile_path(b, -1)) <= 0)
            return 0;
        if (why != NULL && max > 0) {
            strncpy(why, "already running", (size_t) max - 1);
            why[max - 1] = '\0';
        }
        return 1;
    }
    return 0;
}

int
vlhe_apply_account_decide(int trial, long euid, struct vlhe_account *a)
{
    if (trial || euid != 0 || a == NULL)
        return 0;
    if (vlhe_conf_account(VLHE_ACCOUNT, a) != 0)
        return -1;
    /* AN ACCOUNT THAT IS ROOT IS NOT ONE - a hand-made passwd line
     * with uid 0 would make the drop a no-op and say otherwise. */
    if (a->uid <= 0 || a->gid <= 0)
        return -1;
    return 1;
}

int
vlhe_apply_daemon_account(struct vlhe_account *a)
{
    return vlhe_apply_account_decide(vlhe_self_is_trial(),
                                     (long) geteuid(), a);
}

/* The directory part of `path' into `out', or "" for a bare name. */
static void
dir_of(const char *path, char *out, size_t max)
{
    const char *sl = strrchr(path, '/');
    size_t n = (sl != NULL) ? (size_t) (sl - path) : 0;

    if (n >= max)
        n = 0;
    memcpy(out, path, n);
    out[n] = '\0';
}

/*
 * WHAT THE ACCOUNT MUST WRITE, handed over while we are still root.
 * Idempotent: every daemon start does it, and a directory left by a
 * portable run as root - pid files and FIFOs owned by root - is
 * re-owned rather than refused, since the account could neither
 * reopen them nor write its pid.
 */
/*
 * HAND ONE ENTRY TO THE ACCOUNT, BY WHAT IT IS RATHER THAN BY ITS NAME -
 * design/55 recommendation 2 (section 6 row 20), 2026-10-04.
 *
 * The directory these live in belongs to the account, so between root
 * looking at a name and root chowning it the account can put something
 * else there - and chown() follows a symlink, and /var/run usually
 * shares a filesystem with /etc, so a HARD link to a root file is just
 * as available. Either way root would give the account a file it
 * names. So: lstat() first and go no further for anything but a plain
 * file, a FIFO or a directory (so a device node is never even opened);
 * open it O_NOFOLLOW|O_NONBLOCK; insist fstat() sees the same inode and
 * a single link; and fchown() THROUGH THE DESCRIPTOR, so what is chowned
 * is what was checked.
 */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000      /* i386 and x86-64 alike; the target's
                                 * headers give it only under _GNU_SOURCE */
#endif

static void
give_to_account(const char *path, const struct vlhe_account *a)
{
    struct stat ls, fs;
    int fd;

    if (lstat(path, &ls) != 0)
        return;
    if (!S_ISREG(ls.st_mode) && !S_ISFIFO(ls.st_mode) && !S_ISDIR(ls.st_mode))
        return;                 /* a link, a node, a socket: leave it */
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return;
    if (fstat(fd, &fs) == 0
        && fs.st_dev == ls.st_dev && fs.st_ino == ls.st_ino
        && (S_ISDIR(fs.st_mode) || fs.st_nlink == 1))
        (void) fchown(fd, (uid_t) a->uid, (gid_t) a->gid);
    close(fd);
}

/*
 * TAKE A DIRECTORY BACK FOR ROOT - design/55 section 14, design/54 D44,
 * 2026-10-04. Before this the account OWNED /var/run/vlhe and
 * /var/lib/vlhe, and whoever owns a directory can rename or remove
 * anything in it - root's session, baseline, notice, mixers, run lock.
 * Now each is root 0755 and the account has a subdirectory of its own
 * (ctl/, state/). Done through a descriptor with the same checks as
 * give_to_account(): a directory, not a link, the same inode.
 */
static void
take_back_for_root(const char *path)
{
    struct stat ls, fs;
    int fd;

    if (lstat(path, &ls) != 0 || !S_ISDIR(ls.st_mode))
        return;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return;
    if (fstat(fd, &fs) == 0 && S_ISDIR(fs.st_mode)
        && fs.st_dev == ls.st_dev && fs.st_ino == ls.st_ino) {
        if (fs.st_uid != 0 || fs.st_gid != 0)
            (void) fchown(fd, 0, 0);
        if ((fs.st_mode & 07777) != 0755)
            (void) fchmod(fd, 0755);
    }
    close(fd);
}

/* Hand the account one directory and every entry in it - the old
 * account_dirs() loop, now run on ctl/ rather than the run directory. */
static void
give_dir_to_account(const char *dirpath, const struct vlhe_account *a)
{
    DIR *d;
    struct dirent *e;

    give_to_account(dirpath, a);
    d = opendir(dirpath);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        char p[VLHE_PATH_MAX + 64];

        if (e->d_name[0] == '.')
            continue;
        if (strlen(dirpath) + strlen(e->d_name) + 2 > sizeof p)
            continue;
        /* NOT THE RUN LOCK - root's alone (design/54 D51), in case an
         * override has put it here. */
        if (strcmp(e->d_name, "apply.lock") == 0)
            continue;
        sprintf(p, "%s/%s", dirpath, e->d_name);
        /* NEVER THROUGH A SYMLINK - `cdrom' is /dev/cdrom's inner link
         * to /dev/vdiscN: following it would hand the DRIVE NODE to
         * the account. give_to_account() skips links. */
        give_to_account(p, a);
    }
    closedir(d);
}

static void
account_dirs(const struct vlhe_account *a)
{
    const char *run = vlhe_status_rundir();
    const char *ctl = vlhe_status_ctldir();
    char  dir[VLHE_PATH_MAX], parent[VLHE_PATH_MAX];
    DIR  *d;
    struct dirent *e;

    /*
     * THE SPLIT LAYOUT - design/55 section 14. The run directory is
     * root's; ctl/ inside it is the account's. (VLHE_RUNDIR, a test
     * override, names one directory for both, and then the old
     * whole-directory handover below applies.)
     */
    if (run != NULL && ctl != NULL && strcmp(run, ctl) != 0) {
        (void) mkdir(run, 0755);
        take_back_for_root(run);
        (void) mkdir(ctl, 0755);
        give_dir_to_account(ctl, a);
        run = NULL;                     /* done */
    }

    if (run != NULL && run[0] != '\0') {
        (void) mkdir(run, 0755);
        give_to_account(run, a);
        d = opendir(run);
        if (d != NULL) {
            while ((e = readdir(d)) != NULL) {
                char p[VLHE_PATH_MAX + 64];

                if (e->d_name[0] == '.')
                    continue;
                if (strlen(run) + strlen(e->d_name) + 2 > sizeof p)
                    continue;
                /* NOT THE RUN LOCK - root's alone (design/54 D51); the
                 * account never takes it, and owning it let the account
                 * hold Loads off. */
                if (strcmp(e->d_name, "apply.lock") == 0)
                    continue;
                sprintf(p, "%s/%s", run, e->d_name);
                /* NEVER THROUGH A SYMLINK - `cdrom' here is /dev/cdrom's
                 * inner link to /dev/vdiscN (2026-10-03): following it
                 * would hand the DRIVE NODE to the account. The link
                 * itself needs no owner - the directory's decides who
                 * may replace it. give_to_account() skips links and
                 * every other way a name could point elsewhere. */
                give_to_account(p, a);
            }
            closedir(d);
        }
    }

    /* THE DRIVE STATE - vdiscd rewrites it by rename, so the account
     * needs the DIRECTORY, not only the file. */
    dir_of(vlhe_conf_drives_path(), dir, sizeof dir);
    dir_of(dir, parent, sizeof parent);
    /*
     * AND ITS PARENT IS ROOT'S - design/55 section 14. When the state
     * directory is the split layout's state/, /var/lib/vlhe above it
     * holds root's session and baseline: made root 0755, and an older
     * install's /var/lib/vlhe/drives moved into state/ by rename (one
     * filesystem, atomic; never copied). Only for that shape - an
     * override or a portable folder is left exactly as it was.
     */
    if (dir[0] != '\0' && parent[0] != '\0'
        && strlen(dir) > 6 && strcmp(dir + strlen(dir) - 6, "/state") == 0) {
        char old[VLHE_PATH_MAX + 16];
        struct stat so, sn;

        (void) mkdir(parent, 0755);
        take_back_for_root(parent);
        (void) mkdir(dir, 0755);
        if (strlen(parent) + sizeof "/drives" < sizeof old) {
            sprintf(old, "%s/drives", parent);
            if (lstat(old, &so) == 0 && S_ISREG(so.st_mode)
                && lstat(vlhe_conf_drives_path(), &sn) != 0)
                (void) rename(old, vlhe_conf_drives_path());
        }
    }
    if (dir[0] != '\0') {
        (void) mkdir(dir, 0755);
        give_to_account(dir, a);
        /* THE FILE, BY DESCRIPTOR TOO - it had no check at all (design/55
         * row 20: "root chowns an arbitrary file to vlhe, no race
         * needed"), since the account owns the directory it sits in. */
        give_to_account(vlhe_conf_drives_path(), a);
    }
}

/* In the child, as the last thing before execvp. Never returns on
 * failure - a daemon meant to be `vlhe' must not start as root. */
static void
become_account(const struct vlhe_account *a)
{
    gid_t g[VLHE_ACCOUNT_GROUPS];
    int   k;

    account_dirs(a);
    for (k = 0; k < a->ngroups && k < VLHE_ACCOUNT_GROUPS; k++)
        g[k] = (gid_t) a->groups[k];
    if (setgroups((size_t) k, g) != 0
        || setgid((gid_t) a->gid) != 0
        || setuid((uid_t) a->uid) != 0
        || getuid() != (uid_t) a->uid || geteuid() != (uid_t) a->uid
        || getgid() != (gid_t) a->gid) {
        fprintf(stderr, "vlhe: could not become the `%s' account (uid"
                        " %ld) - %s. Not starting the daemon as root.\n",
                VLHE_ACCOUNT, a->uid, strerror(errno));
        _exit(126);
    }
}

/*
 * SAID IN THE PROGRESS, after the command line, so a log shows who
 * each daemon was - and loudly when an installed machine has no
 * account, which is a broken install rather than a choice.
 */
static void
note_account(FILE *out, const char *cmdbuf, int daemon)
{
    struct vlhe_account a;
    char  first[VLHE_PATH_MAX];
    size_t n;
    int   r;

    if (out == NULL || !daemon || cmdbuf == NULL)
        return;
    n = strcspn(cmdbuf, " ");
    if (n >= sizeof first)
        return;
    memcpy(first, cmdbuf, n);
    first[n] = '\0';
    if (!vlhe_apply_is_our_daemon(first))
        return;
    r = vlhe_apply_daemon_account(&a);
    if (r == 1)
        fprintf(out, "#   as the %s account (uid %ld, gid %ld)\n",
                VLHE_ACCOUNT, a.uid, a.gid);
    else if (r < 0)
        fprintf(out, "#   NO `%s' ACCOUNT - started as root. The"
                     " install should have made it: reinstall, or see"
                     " INSTALL (design/33 section 3k)\n", VLHE_ACCOUNT);
}

static int run_expanded(char *buf, int background);
static int run_expanded_t(char *buf, int background, int timeout_s);
static void remember_child(pid_t pid);

/*
 * A COMMAND THAT DOES NOT FINISH NO LONGER HANGS THE PRESS - design/54
 * D61, 2026-10-04 (the user: "those time outs are fine").
 *
 * The foreground wait was a plain waitpid(kid, ..., 0): an insmod stuck
 * in a module's init, a modprobe waiting on something, froze the GUI
 * for as long as it lasted. Now the wait has a deadline - 30 s for the
 * module tools, 5 s for anything else (the `killall -q -0' checks are
 * the many) - and at the deadline the child gets SIGTERM, then SIGKILL,
 * and the step reports RUN_TIMEOUT, which the plan journals as NOT
 * DONE.
 *
 * AN alarm() AROUND THE BLOCKING waitpid(), NOT A POLL: a Load runs
 * some fifty short commands, and a WNOHANG poll would add a tick (10 ms
 * at HZ=100) to every one. The alarm's handler does nothing; its only
 * job is to make waitpid() return EINTR, installed without SA_RESTART
 * and put back afterwards. Nothing else in the GUI uses SIGALRM.
 *
 * ALWAYS A SAVED pid, GUARDED > 0, NEVER waitpid(-1) OR A GROUP KILL -
 * CLAUDE.md section 1. An insmod stuck inside init_module is in D state
 * and SIGKILL cannot reach it: then the pid is remembered for reaping
 * later, the press returns, and the journal says the machine needs
 * attention. Killing a modprobe may leave the insmod it started; that
 * is accepted rather than signal a process group.
 */
#define RUN_TIMEOUT     (-2)
#define RUN_MODULE_S    30
#define RUN_OTHER_S     5

static int g_last_timeout_s;            /* the deadline that ran out */
static volatile int g_alarm_fired;

static void
on_run_alarm(int sig)
{
    (void) sig;
    g_alarm_fired = 1;
}

/* 0 reaped, with *status; -1 waitpid failed; RUN_TIMEOUT it did not
 * finish and was stopped (or left for reaping). */
static int
wait_bounded(pid_t saved, int *status, int timeout_s)
{
    struct sigaction sa, old;
    pid_t got;
    int   i;

    if (saved <= 0)
        return -1;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_run_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                    /* NOT SA_RESTART */
    g_alarm_fired = 0;
    sigaction(SIGALRM, &sa, &old);
    alarm((unsigned) timeout_s);

    for (;;) {
        got = waitpid(saved, status, 0);
        if (got == saved)
            break;
        if (got < 0 && errno == EINTR && !g_alarm_fired)
            continue;                   /* some other signal */
        break;
    }
    alarm(0);
    sigaction(SIGALRM, &old, NULL);

    if (got == saved)
        return 0;
    if (!g_alarm_fired)
        return -1;

    /* THE DEADLINE PASSED. TERM, two seconds; KILL, one more. */
    g_last_timeout_s = timeout_s;
    if (saved > 0)
        kill(saved, SIGTERM);
    for (i = 0; i < 20; i++) {
        if (waitpid(saved, status, WNOHANG) == saved)
            return RUN_TIMEOUT;
        usleep(100000);
    }
    if (saved > 0)
        kill(saved, SIGKILL);
    for (i = 0; i < 10; i++) {
        if (waitpid(saved, status, WNOHANG) == saved)
            return RUN_TIMEOUT;
        usleep(100000);
    }
    remember_child(saved);              /* D state: reap it one day */
    return RUN_TIMEOUT;
}

/* WHICH DEADLINE - by the command's own name, so the plan's steps and
 * the stray checks need not say. */
static int
run_deadline(const char *argv0)
{
    const char *b;

    if (argv0 == NULL)
        return RUN_OTHER_S;
    b = strrchr(argv0, '/');
    b = (b != NULL) ? b + 1 : argv0;
    if (strcmp(b, "insmod") == 0 || strcmp(b, "modprobe") == 0
        || strcmp(b, "rmmod") == 0)
        return RUN_MODULE_S;
    return RUN_OTHER_S;
}

/* FOR THE HOST TESTS: a command with a chosen deadline, its rc as the
 * runner would see it (RUN_TIMEOUT = -2). No node expansion. */
int
vlhe_apply_run_for_test(const char *cmd, int timeout_s)
{
    char buf[VLHE_CMD_MAX];

    strncpy(buf, cmd, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    return run_expanded_t(buf, 0, timeout_s);
}

static int
run_command(const char *cmd, int background)
{
    char buf[VLHE_CMD_MAX];

    strncpy(buf, cmd, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    expand_nodes(buf, sizeof buf, cmd_resolves_own_out(cmd));
    return run_expanded(buf, background);
}

/* THE SECOND HALF, taking the line expand_nodes() already produced -
 * so the runner can print exactly what it is about to execute and
 * then execute those bytes, without expanding (and probing) twice.
 * `buf' is split in place. */
static int
run_expanded(char *buf, int background)
{
    return run_expanded_t(buf, background, 0);
}

/* `timeout_s' 0 means "by the command's name" - run_deadline(). */
static int
run_expanded_t(char *buf, int background, int timeout_s)
{
    char *argv[32];
    int   argc = 0;
    pid_t kid;
    int   status = 0;

    argc = split_args(buf, argv, 31);
    if (argc == 0)
        return -1;

    kid = fork();
    if (kid < 0)
        return -1;

    if (kid == 0) {
        /*
         * A DAEMON GETS ITS OWN SESSION. None of ours calls
         * setsid() or forks - load.sh has always started them with
         * `&' - so without this they stay in our process group and
         * die with the terminal that ran `vlhe apply'.
         */
        if (background) {
            int fd;

            setsid();
            /*
             * AND ITS OUTPUT GOES TO A LOG, NOT THE TERMINAL.
             *
             * setsid() detaches the SESSION and inherits the FILE
             * DESCRIPTORS, which is not the same thing - so until
             * 2026-09-21 every daemon `vlhe apply' started wrote to
             * whatever tty launched it. The user saw
             *
             *     nova50:/mnt/xfer# vsoundd: 500 writes, 0 slow ...
             *
             * interleaved with their shell prompt, and worse, NOTHING
             * WAS CAPTURED AT ALL: a daemon that fails after startup
             * says so into a terminal that later goes away.
             *
             * load.sh has always done this - `./vsoundd ... >>
             * DAEMON.LOG 2>&1 &' (load.sh:947) - and the executor
             * simply did not. Three daemon failures in one evening
             * were invisible for exactly this reason: vmidid's
             * -EBUSY, vmidid's `-P 32' usage dump, and vdiscd's empty
             * drive list all scrolled past a console nobody was
             * reading.
             *
             * DAEMON.LOG BESIDE THE JOURNAL, so one directory holds
             * what changed and what the daemons then said about it.
             * Appended: several daemons share the file and a run may
             * follow another.
             *
             * A FAILURE HERE IS NOT FATAL. If the log cannot be
             * opened the daemon still starts - with its output going
             * nowhere, which is what /dev/null is for. Refusing to
             * start audio because a log file is unwritable would be
             * the tail wagging the dog, and it is the same call
             * vlhe_journal_begin() makes.
             *
             * `fd' IS DECLARED AT THE TOP OF THE CHILD BLOCK - C89,
             * no declarations after statements.
             */
            fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) {
                dup2(fd, 0);
                if (fd > 2)
                    close(fd);
            }

            fd = open(vlhe_daemon_log_path(), O_WRONLY | O_CREAT | O_APPEND,
                      0644);
            if (fd < 0)
                fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
                if (fd > 2)
                    close(fd);
            }

            /* AND WHO IT RUNS AS - design/33 section 3k. After the
             * log is open, which is root's file and stays our fd. */
            {
                struct vlhe_account acct;

                if (vlhe_apply_is_our_daemon(argv[0])
                    && vlhe_apply_daemon_account(&acct) == 1)
                    become_account(&acct);
            }
        }
        execvp(argv[0], argv);
        /* THE CHILD MUST NOT RETURN. _exit, not exit, so no atexit
         * handler or stdio buffer of the parent's runs twice. */
        _exit(127);
    }

    /*
     * A DAEMON IS NOT WAITED FOR, AND THAT IS THE WHOLE POINT.
     *
     * FOUND ON TARGET 2026-09-21, second real run. The plan reached
     * `/mnt/xfer/vsoundd -d /dev/dsp1', the pump started and
     * announced itself - and `vlhe apply' HUNG THERE. vmidid and
     * vdiscd never ran.
     *
     * None of our daemons daemonises: no fork, no setsid, they run
     * in the foreground and load.sh has always started them with
     * `&'. So waitpid() on one waits until it exits, which for a
     * working daemon is never.
     *
     * The step is reported as started rather than as succeeded,
     * because that is all that can be known without waiting. A
     * daemon that dies a second later shows up as a missing pidfile
     * on the Status page, which is the right place for it.
     *
     * GUARDED, AND ON A SAVED PID, for the modules we DO wait on.
     * CLAUDE.md section 1 records kill(-1, SIGKILL) taking the
     * user's X session twice from a pid field that had been reset.
     * Nothing here signals, but the same discipline applies to
     * waitpid: a pid of -1 would wait for ANY child, which in a GUI
     * that has forked something else is the wrong one.
     */
    if (background) {
        /*
         * ONE NON-BLOCKING LOOK, so a daemon that dies INSTANTLY is
         * not reported as started. `vsoundd -d /dev/dsp1' against a
         * busy card, or a flag the binary does not know, exits in
         * milliseconds - and without this the plan would carry on
         * and the user would find out from an empty Status page.
         *
         * A SHORT SLEEP AND ONE waitpid(WNOHANG), NOT A WAIT. The
         * question is "did it fall over on the spot", and anything
         * longer starts guessing at how long a daemon takes to
         * settle - which is the Status page's job, not this
         * function's.
         *
         * A daemon still alive here is reported as started, which is
         * all that can honestly be claimed.
         */
        pid_t got;

        sleep(1);
        got = waitpid(kid, &status, WNOHANG);
        if (got == kid && WIFEXITED(status) && WEXITSTATUS(status) != 0)
            return WEXITSTATUS(status);
        if (got == kid && !WIFEXITED(status))
            return -1;          /* signalled */
        /*
         * AND AN EXIT OF 0 IS NOT "STARTED" EITHER - design/47 A3.
         * None of our daemons detaches with a fork (vsoundd, vdiscd
         * and vmidid all stay in the foreground, which is why the
         * pid is kept), so a child already reaped here has stopped,
         * whatever it said on the way out - and it was being reported
         * as started and REMEMBERED as a live child to reap one day.
         */
        if (got == kid)
            return -1;          /* exited 0 - gone, not running */
        remember_child(kid);    /* alive, and ours to reap one day */
        return 0;
    }

    if (kid > 0) {
        int w = wait_bounded(kid, &status,
                             timeout_s > 0 ? timeout_s
                                           : run_deadline(argv[0]));

        if (w == RUN_TIMEOUT)
            return RUN_TIMEOUT;
        if (w < 0)
            return -1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;                  /* signalled, or stopped */
}

/*
 * THE FAILURE PATH: RUN WHAT MUST STILL BE RUN.
 *
 * Called from every point where vlhe_plan_run() gives up, with the
 * index of the step that failed. It walks the REST of the plan and
 * executes only the steps marked `cleanup' - see vlhe_apply.h for
 * why that flag exists and what it cost not to have it.
 *
 * WHY THE WHOLE REMAINDER RATHER THAN A REMEMBERED UNDO LIST. The
 * plan already contains the restore, in the right order, with the
 * right arguments - built by the same code that built the removal.
 * A separate undo stack would be a second description of the same
 * thing, free to disagree with it.
 *
 * FAILURES HERE ARE REPORTED AND IGNORED. We are already failing;
 * the caller's return code describes the step that broke the plan,
 * not the cleanup. A cleanup that itself fails leaves the machine no
 * worse than doing nothing would have, and saying so is all that can
 * usefully be done.
 *
 * NOTES, REFUSALS AND STOPS ARE NOT CLEANUP and never carry the
 * flag, so the parenthesised forms cannot reach execvp from here.
 * The flag is only ever set on a real command.
 */
/*
 * PUT BACK EVERY /dev/dsp-SIDE LINK THE SESSION RECORDS AS STILL TO UNDO
 * - OPEN or FAILED (design/54 7h, the retry). One function for the two
 * places that did it - the unload's `(dsprestore)' and the failure
 * path's `run_cleanup()' - which were copies, and both took OPEN only.
 * `/dev/cdrom' is a link record too and belongs to `(cdromrestore)'.
 * Returns how many it acted on, or -1 when the file cannot be read.
 */
static int
restore_recorded_links(FILE *out)
{
    struct vlhe_sess_rec *rec;
    int n, i, did = 0;

    rec = vlhe_session_read_all(&n);
    if (n < 0)
        return -1;
    for (i = n - 1; i >= 0; i--) {
        char made[VLHE_PATH_MAX];
        int  r;

        if (rec[i].kind != VLHE_SESS_LINK || !rec_pending(&rec[i]))
            continue;
        if (strcmp(rec[i].what, "/dev/cdrom") == 0)
            continue;
        did++;
        /* link_dsp_restore() prints the command once it has decided
         * to run it. */
        note_made(rec[i].note, made, sizeof made);
        r = link_dsp_restore(rec[i].what, rec[i].prior, made, out);
        (void) vlhe_session_mark(i,
            r == 0 ? VLHE_SESS_UNDONE
            : r == 2 ? VLHE_SESS_CONFLICT : VLHE_SESS_FAILED,
            r == 0 ? NULL : r == 2
            ? "changed while loaded - left as found"
            : "could not restore");
    }
    free(rec);
    return did;
}

static void
run_cleanup(const struct vlhe_plan *p, int from, FILE *out)
{
    int  i, any = 0;
    char cmdbuf[VLHE_CMD_MAX];  /* the expanded command, printed then run */

    for (i = from; i < p->n; i++) {
        const struct vlhe_step *st = &p->step[i];
        int rc;

        if (!st->cleanup)
            continue;

        /*
         * A PARENTHESISED STEP IS NOT A COMMAND, AND THIS LOOP DID
         * NOT KNOW IT. Found on target 2026-09-23, first run of the
         * cleanup path:
         *
         *     undo  modprobe sb io=0x220 irq=5 dma=1 dma16=5
         *     undo  (cdromrestore)
         *           cleanup failed rc=127 - the machine may need
         *           attention
         *
         * TWO `undo' LINES AND ONE FAILURE. The 127 is our own
         * _exit() after execvp, and it belonged to `(cdromrestore)' -
         * execvp looked for a program of that literal name. **The
         * card had already come back**, confirmed by lsmod; the
         * message was about the wrong step and said the machine
         * needed attention when it did not.
         *
         * THE PARENTHESIS CONVENTION EXISTS FOR EXACTLY THIS - see
         * step_is_note() and the "(stop ...)" comment in the runner -
         * and run_cleanup() is new enough (2026-09-23) that it never
         * got the check every other execution path has.
         *
         * AND `(cdromrestore)' IS NOW OURS TO RUN - CHANGED
         * 2026-09-24 WITH THE FIX FOR design/36 ROW 51.
         *
         * THIS COMMENT USED TO SAY the runner reached it "through the
         * normal loop before this ever sees it", and that was true
         * and was the bug: the normal loop ran it on the SUCCESS path
         * as well, undoing the redirect one step after making it. The
         * loop now skips parenthesised cleanup steps, so if this
         * function does not act on the step, nothing does and a
         * FAILED load leaves `/dev/cdrom' pointing at a virtual drive
         * that never came up - the exact state the step exists to
         * prevent.
         *
         * The other notes are still skipped: `(stop X)' and the rest
         * are forward-path pseudo-steps with nothing to undo.
         */
        if (strcmp(st->cmd, "(dsprestore)") == 0) {
            /*
             * FROM THE SESSION FILE, NOT FROM THE CONFIG - and this
             * line is the whole of design/36 rows 75 and 77.
             *
             * IT USED TO READ `so.programs_use' HERE, i.e. whatever
             * the settings say AT UNLOAD TIME. Change the setting
             * between the load and the unload and the restore was
             * aimed at a path the load had never touched, while the
             * one it HAD touched stayed a symlink for ever. On
             * 2026-09-25 that left `/dev/dsp', `/dev/dsp1' and
             * `/dev/dsp2' all pointing at a module that was gone and
             * the control centre could find no sound card at all.
             *
             * THE SESSION FILE CANNOT MAKE THAT MISTAKE because each
             * record carries the path the load actually changed.
             * Nothing here consults a setting.
             *
             * BACKWARDS, so changes come off in the reverse of the
             * order they went on.
             */
            {
                int did = restore_recorded_links(out);

                if (did < 0) {
                    /* COULD NOT READ IT - say so. A silent skip here
                     * is exactly what let six unloads in a row report
                     * success while leaving a symlink behind. */
                    if (out != NULL)
                        fprintf(out, "#   %s: cannot read - the undo"
                                     " list is unavailable\n",
                                vlhe_session_path());
                } else if (did == 0 && out != NULL) {
                    /* NOTHING RECORDED, AND NOTHING TO GUESS WITH -
                     * since `dsp.was' went (2026-09-25) there is no
                     * prior state to restore to without a record. */
                    fprintf(out, "#   nothing recorded - no link of"
                                 " ours to put back\n");
                }
            }
            continue;
        }
        if (strcmp(st->cmd, "(cdromrestore)") == 0) {
            struct vlhe_cdrom_link ln;

            /* NOTHING SAVED, NOTHING TO PUT BACK - and silent, like
             * the runner's own branch, so a machine that never used
             * the option prints no line. */
            if (vlhe_cdrom_link(&ln) != 0) {
                /* CANNOT READ ITS STATE - design/47 section 5: this
                 * fell through and attempted a restore of nothing. */
                if (out != NULL)
                    fprintf(out, "#   /dev/cdrom: cannot read its state -"
                                 " not restored\n");
                continue;
            }
            if (ln.saved[0] == '\0')
                continue;

            if (!any) {
                any = 1;
                if (out != NULL)
                    fprintf(out, "#---- putting things back ----\n");
            }
            if (out != NULL) {
                shell_cdromrestore(out, ln.saved);
                fflush(out);
            }
            if (link_cdrom_restore(out) != 0 && out != NULL)
                fprintf(out, "#   could not restore /dev/cdrom -"
                             " it may still point at the virtual"
                             " drive\n");
            continue;
        }

        if (step_is_note(st->cmd)) {
            if (out != NULL)
                fprintf(out, "#   %s\n", st->cmd);
            continue;
        }

        if (!any) {
            any = 1;
            if (out != NULL)
                fprintf(out, "#---- putting things back ----\n");
        }

        strncpy(cmdbuf, st->cmd, sizeof cmdbuf - 1);
        cmdbuf[sizeof cmdbuf - 1] = '\0';
        expand_nodes(cmdbuf, sizeof cmdbuf, cmd_resolves_own_out(st->cmd));
        if (out != NULL) {
            shell_expanded(out, cmdbuf, st->kind == VLHE_STEP_DAEMON);
            note_account(out, cmdbuf, st->kind == VLHE_STEP_DAEMON);
            fflush(out);
        }

        rc = run_expanded(cmdbuf, st->kind == VLHE_STEP_DAEMON);
        if (rc == 0)
            journal_step(st, out);
        else if (out != NULL)
            fprintf(out, "#   cleanup failed rc=%d - the machine"
                         " may need attention\n", rc);
    }
}

/*
 * THE LAST RESORT: `killall -w', AND IT MUST SAY IT WAS USED.
 *
 * THE USER'S CALL, 2026-09-23, and the reasoning is theirs and better
 * than the one it replaced: "I do think killall earns it place. but
 * it would have hidden the failures we ran into."
 *
 * THAT IS EXACTLY RIGHT, AND IT DECIDES WHERE THIS GOES RATHER THAN
 * WHETHER. Every bug found today surfaced because the precise path
 * FAILED VISIBLY - vmidid reporting "another reader holds it", the
 * journal recording "unload module vmidi *** NOT DONE", `vdisc 24372
 * 1' left in lsmod. A killall in the normal teardown would have
 * quietly cleaned up after all of it, and the pidfile-overwrite bug -
 * which had probably been there since the daemons were written -
 * would have shipped.
 *
 * SO: THE PIDFILE PATH RUNS FIRST AND ITS FAILURE IS RECORDED. This
 * runs only after that, and it journals what it did. A stray should
 * be RARE now that a daemon refuses to start twice; if this line
 * starts appearing regularly, that is itself the finding.
 *
 * ALL THREE DAEMONS ARE SINGLE-INSTANCE BY DESIGN, enforced in the
 * kernel rather than by convention - vmidi_mod.c's `vmidi_opened'
 * flag, /dev/vdiscctl admitting one opener, and one pump at a time
 * since 6ac4317. So a second copy is not a configuration we might be
 * trampling: it CANNOT WORK, and a stray is holding a device nobody
 * can use.
 *
 * `killall' HERE IS psmisc 17 - checked on the target's own image,
 * not assumed. It matches by NAME, and `killall5' (the kill-
 * everything one, which is what the name means on some older
 * systems) is a separate binary in /sbin. `-w' waits for them to die,
 * which the rmmod that follows needs.
 *
 * Returns 1 if it killed something, 0 if there was nothing to kill.
 */
/*
 * THE DAEMONS THIS PROCESS STARTED, so a stray that is OUR OWN ZOMBIE
 * can be reaped by the one process able to do it. Sixteen is more than
 * the plan can ever start; a full table just means an old entry is
 * dropped, and a dropped entry costs nothing worse than the pre-fix
 * behaviour for that one pid.
 */
#define VLHE_MAX_CHILDREN 16
static pid_t g_children[VLHE_MAX_CHILDREN];
static int   g_nchildren;

static void
remember_child(pid_t pid)
{
    if (pid <= 0)
        return;
    if (g_nchildren == VLHE_MAX_CHILDREN) {
        memmove(g_children, g_children + 1,
                (VLHE_MAX_CHILDREN - 1) * sizeof g_children[0]);
        g_nchildren--;
    }
    g_children[g_nchildren++] = pid;
}

/* waitpid(WNOHANG) on each remembered child - NEVER waitpid(-1), which
 * in a GUI would collect a render's child out from under the code
 * waiting for it (the comment in run_command() says the same). A child
 * that has exited is reaped and forgotten; a live one is left alone. */
static int
reap_our_children(void)
{
    int i = 0, reaped = 0;

    while (i < g_nchildren) {
        int status;

        if (waitpid(g_children[i], &status, WNOHANG) == g_children[i]) {
            reaped++;
            g_children[i] = g_children[--g_nchildren];
            continue;
        }
        i++;
    }
    return reaped;
}

static int
killall_stray(const char *name, FILE *out)
{
    char cmd[96];
    int  i, sig = 15;

    /*
     * THE OLD VERSION WAS `killall -w -q NAME' AND IT DEADLOCKED THE
     * GUI, 2026-09-24, design/36 row 46.
     *
     * `-w' polls kill(pid, 0) and drops a pid only on ESRCH
     * (psmisc 17, killall.c:180-189 - the exact version on the
     * target, read from the Corel source disc). A ZOMBIE still answers
     * kill(pid, 0), so -w never returned; and the zombie was OUR
     * child, started by run_command(), so the only process able to
     * reap it was the one blocked in waitpid() waiting for killall.
     * `ps' in the guest: 533 Z [vmidid <defunct>], 557 S killall -w -q
     * vmidid, 488 S vlhe.gtk. Broken by hand with kill -9 on the
     * killall.
     *
     * killall ITSELF IS THE RIGHT TOOL - the user's point: it matches
     * by NAME, so it reaches a stray started from a shell as well as
     * one of ours, and nothing else does. Only the WAIT was wrong. So
     * the wait is ours now:
     *
     *   1. `killall -q -0 NAME'   is anything there at all? Exit status
     *                             is "did a name match", independent
     *                             of -q (killall.c:174), so 0 = yes.
     *   2. `killall -q NAME'      SIGTERM, no -w.
     *   3. poll: reap OUR exited children, ask -0 again, 100 ms apart;
     *      at 3 s escalate with `killall -q -9 NAME' and keep polling
     *      to 5 s. The escalation did not exist before - vlhe_apply.c
     *      said "killall did not reach it either" and had never sent
     *      anything but TERM.
     *
     * A stray is RARE now that a daemon refuses to start twice, so
     * every rung is journalled: the pid file did not account for a
     * process, and which signal it took is part of what went wrong.
     *
     * `-0' and `-9' are numeric on purpose: psmisc's get_signal() is
     * isdigit() -> atoi(), read in signals.c, so both parse.
     */
    /* run_command() answers the EXIT STATUS, or -1 when nothing ran:
     * killall -0 exits 1 for "nothing matched", 127 when there is no
     * killall to run (execvp failed in the child). The old `!= 0'
     * read a missing killall as no stray - design/47 section 5. */
    sprintf(cmd, "killall -q -0 %.40s", name);
    {
        int rc = run_command(cmd, 0);

        if (rc == 127 || rc < 0) {
            if (out != NULL)
                fprintf(out, "#   killall is not available - a stray %s"
                             " cannot be checked for\n", name);
            return 0;
        }
        if (rc != 0) {
            reap_our_children();        /* a zombie of ours matches -0 */
            sprintf(cmd, "killall -q -0 %.40s", name);
            if (run_command(cmd, 0) != 0)
                return 0;               /* nothing matched - the usual answer */
        }
    }

    sprintf(cmd, "killall -q %.40s", name);
    run_command(cmd, 0);

    for (i = 0; i < 50; i++) {
        reap_our_children();
        sprintf(cmd, "killall -q -0 %.40s", name);
        if (run_command(cmd, 0) != 0)
            break;                      /* gone */
        if (i == 30) {
            sprintf(cmd, "killall -q -9 %.40s", name);
            run_command(cmd, 0);
            sig = 9;
        }
        usleep(100000);
    }

    if (i >= 50) {
        if (out != NULL)
            fprintf(out, "#   a STRAY %s is STILL RUNNING after"
                         " SIGKILL - left alone\n", name);
        vlhe_journal_failed(VLHE_CH_DAEMON_STOP, name,
                            "STRAY - survived killall -9");
        return 0;
    }

    if (out != NULL)
        fprintf(out, "#   a STRAY %s was killed by name (SIG%s,"
                     " after %d ms) - the pid file did not account"
                     " for it\n",
                name, sig == 9 ? "KILL" : "TERM", i * 100);

    /*
     * JOURNALLED AS A CHANGE, NOT AS A FAILURE. Something WAS stopped,
     * so the machine did change and the line must say so - but the
     * detail records HOW, because that is the part worth noticing.
     */
    vlhe_journal_add(VLHE_CH_DAEMON_STOP, name,
                     sig == 9
                       ? "STRAY - killed by name with SIGKILL, not by pid file"
                       : "STRAY - killed by name, not by pid file");
    return 1;
}

static int plan_run_raised(const struct vlhe_plan *p, FILE *out);

/* ------------------------------------------------------------------ */
/* The shell rendering - ONE VOCABULARY FOR THE SIMULATION AND THE RUN */
/* ------------------------------------------------------------------ */

/*
 * THE RUN PRINTS WHAT THE SIMULATION PRINTS, WITH THE OUTCOME UNDER
 * IT - the user, 2026-10-01: "Can the Load and Unload window that
 * tells you what changes match the Simulate output so it shows
 * everything that was done the commands and everything?"
 *
 * Until then the run wrote `  nodes (dspnode)' and `  start vsoundd
 * -d @CARD@ ...' - the step's own text, tags unresolved, pseudo-steps
 * as a word - while the simulation wrote `mknod /dev/dsp1 c 14 19'
 * and the resolved command. Two vocabularies for the same ten steps,
 * and a user who had read the simulation could not find it in the
 * result box.
 *
 * SO EVERY LINE A RUN WRITES IS EITHER A COMMAND OR A `#' COMMENT,
 * exactly as the simulation's are. A real command is printed as it
 * is about to be executed (expanded, quoted, `&' on a daemon); a
 * pseudo-step prints the shell that does the same thing, with the
 * REAL values the step is about to use where the simulation could
 * only assume them; and everything that was prose - "created",
 * "FAILED - stopping", "already there" - is a comment beneath. The
 * per-component headings are the simulation's. A saved run reads
 * as the simulation with its outcomes attached, and the same
 * functions below write both, so they cannot drift.
 *
 * THE SAME ON THE CLI, deliberately: `vlhe apply' and the Load
 * button run the same code, nothing parses the output (vlhe-init
 * discards it, the tests assert on the simulation), and a user who
 * saved `vlhe simulate' can diff it against the run only if the two
 * share a vocabulary.
 */

/* `#---- vsound ----' when the component changes. `last' is the
 * caller's, starting at -1. */
static void
shell_heading(FILE *fp, int comp, int *last)
{
    if (fp == NULL || comp == *last)
        return;
    fprintf(fp, "#---- %s ----\n",
            comp == VLHE_ENABLE_SOUND ? "vsound"
          : comp == VLHE_ENABLE_MIDI  ? "vmidi"
          : comp == VLHE_ENABLE_CD    ? "vdisc"
          : "the sound core");
    *last = comp;
}

/* vsound's node and the name programs reach it by. `idx' is which
 * /dev/dspN vsound took; `use' is the name redirected at it. */
static void
shell_dspnode(FILE *fp, int idx, const char *use)
{
    if (fp == NULL)
        return;
    fprintf(fp, "[ -e /dev/dsp0 ] || mknod /dev/dsp0 c 14 3\n");
    if (idx > 0)
        fprintf(fp, "[ -e /dev/dsp%d ] || mknod /dev/dsp%d c 14 %d\n",
                idx, idx, 3 + 16 * idx);
    if (idx > 0 && use != NULL && use[0] != '\0')
        fprintf(fp, "rm -f %s && ln -s dsp%d %s\n", use, idx, use);
}

static void
shell_vmidinode(FILE *fp, int minor)
{
    if (fp != NULL)
        fprintf(fp, "rm -f /dev/vmidi && mknod /dev/vmidi c 14 %d\n", minor);
}

static void
shell_vdisc_nodes(FILE *fp, int major, int n)
{
    int i;

    if (fp == NULL)
        return;
    if (n < 1)
        n = 1;
    /* A MAJOR OF 0 IS "WHATEVER THE KERNEL GAVE" - design/47 L4 - and
     * the shell for that is the same question /proc/devices answers,
     * as the control node's minor is asked of /proc/misc below. The
     * runner resolves it from the same file. */
    for (i = 0; i < n; i++) {
        if (major > 0)
            fprintf(fp, "rm -f /dev/vdisc%d && mknod /dev/vdisc%d b %d %d\n",
                    i, i, major, i);
        else
            fprintf(fp, "rm -f /dev/vdisc%d && mknod /dev/vdisc%d b"
                        " `awk '/ vdisc$/ { print $1 }' /proc/devices` %d\n",
                    i, i, i);
    }
    fprintf(fp, "rm -f /dev/vdiscctl && mknod /dev/vdiscctl c 10"
                " `awk '/vdiscctl/ { print $1 }' /proc/misc`\n");
}

static void
shell_rmnodes_cd(FILE *fp, int n)
{
    int i;

    if (fp == NULL)
        return;
    if (n < 1)
        n = 1;
    fprintf(fp, "rm -f");
    for (i = 0; i < n; i++)
        fprintf(fp, " /dev/vdisc%d", i);
    fprintf(fp, " /dev/vdiscctl\n");
}

static void
shell_rmnodes_midi(FILE *fp)
{
    if (fp != NULL)
        fprintf(fp, "rm -f /dev/vmidi\n");
}

static void
shell_cdromlink(FILE *fp)
{
    /* TWO LINKS - /dev/cdrom through the inner one in the run
     * directory, which names the drive (link_cdrom_set()). */
    if (fp != NULL) {
        char dir[512];
        const char *in = vdiscd_ctl_cdrom();

        dir_of(in, dir, sizeof dir);
        fprintf(fp, "mkdir -p %s && ln -sfn /dev/vdisc%d %s\n", dir,
                cdrom_drive_recorded(), in);
        fprintf(fp, "rm -f /dev/cdrom && ln -s %s /dev/cdrom\n", in);
    }
}

/* `saved' is what /dev/cdrom pointed at before the load; NULL when
 * that is not known (the simulation), where only a comment can say
 * what the step does. */
static void
shell_cdromrestore(FILE *fp, const char *saved)
{
    if (fp == NULL)
        return;
    if (saved != NULL && saved[0] != '\0')
        fprintf(fp, "rm -f /dev/cdrom && ln -s %s /dev/cdrom\n", saved);
    else
        fprintf(fp, "# /dev/cdrom: put it back to what it pointed at"
                    " before the load, if the load changed it\n");
}

/* A session record put back: `prior' is "node M m", "link TARGET" or
 * empty (the name was free before the load borrowed it). */
static void
shell_restore(FILE *fp, const char *path, const char *prior)
{
    int maj, min;

    if (fp == NULL)
        return;
    if (prior != NULL && sscanf(prior, "node %d %d", &maj, &min) == 2)
        fprintf(fp, "rm -f %s && mknod %s c %d %d\n", path, path, maj, min);
    else if (prior != NULL && strncmp(prior, "link ", 5) == 0)
        fprintf(fp, "rm -f %s && ln -s %s %s\n", path, prior + 5, path);
    else
        fprintf(fp, "rm -f %s\n", path);
}

static void
shell_stop(FILE *fp, const char *name)
{
    if (fp != NULL)
        fprintf(fp, "kill `cat %s`\n", vlhe_status_pidfile_path(name, -1));
}

/*
 * A REAL COMMAND, AS IT IS ABOUT TO RUN. `buf' is the line
 * expand_nodes() produced - the one run_expanded() will execute - so
 * what is printed and what is run are the same bytes. An argument
 * holding ARG_SPACE (a value with a space in it) is printed quoted,
 * which is what a shell would need; a daemon gets its `&'.
 */
static void
shell_expanded(FILE *fp, const char *buf, int daemon)
{
    const char *a = buf;

    if (fp == NULL)
        return;
    while (*a != '\0') {
        const char *e = a;
        int         q = 0;

        while (*e != '\0' && *e != ' ') {
            if (*e == ARG_SPACE)
                q = 1;
            e++;
        }
        if (q)
            fputc('"', fp);
        for (; a < e; a++)
            fputc(*a == ARG_SPACE ? ' ' : *a, fp);
        if (q)
            fputc('"', fp);
        while (*a == ' ')
            a++;
        if (*a != '\0')
            fputc(' ', fp);
    }
    fprintf(fp, "%s\n", daemon ? " &" : "");
}

/*
 * THE WHOLE PLAN RUNS RAISED - design/48 S4. Every step is root's
 * work (insmod, rmmod, daemons, /etc, /dev), and the runner never
 * calls back into the GUI: progress goes to a FILE and the one
 * callback, the relink question, is registered by nobody in the GUI.
 * vlhe_restart() runs through here, so it is covered too.
 *
 * In the CLI and the ordinary build no hooks are registered and
 * this is exactly the old function.
 */
/*
 * ONE LOAD OR UNLOAD AT A TIME - design/54 7h Stage 1, 2026-10-03.
 *
 * The GUI and `vlhe apply' could run at once and interleave their
 * changes and their session records. An fcntl() lock on
 * <run dir>/apply.lock (2.2 has POSIX locks - fs/locks.c): the kernel
 * drops it when the process dies, so nothing is left to read as
 * "still locked" after a crash or a reboot - which a pid file would
 * get wrong when a new process happens to have the old pid. The pid is
 * written in the file for the message only.
 *
 * NESTED, so the GUI can hold it across its three scoped plans and
 * vlhe_plan_run() take it again inside: a count, and the fd closed at
 * zero (closing any fd on the file would drop a POSIX lock). CLOSE ON
 * EXEC, so the daemons a plan starts do not carry the descriptor.
 *
 * Returns 0 held, 1 another process holds it (`who' gets its pid),
 * -1 the file could not be made or locked for another reason - the
 * caller warns and goes on rather than refuse a run over a lock file.
 */
/*
 * WHICH COMPONENT A SESSION RECORD BELONGS TO - design/54 7h Stage 2.
 * Only device-node and link records are written, so the path says it:
 * the vdisc nodes and /dev/cdrom are CD's, /dev/vmidi is MIDI's, and
 * everything else - /dev/dsp, the card's own name, a ProgramsUse path -
 * is Sound's.
 */
static int
record_component(const struct vlhe_sess_rec *r)
{
    if (strncmp(r->what, "/dev/vdisc", 10) == 0
        || strcmp(r->what, "/dev/cdrom") == 0)
        return VLHE_ENABLE_CD;
    if (strcmp(r->what, "/dev/vmidi") == 0)
        return VLHE_ENABLE_MIDI;
    return VLHE_ENABLE_SOUND;
}

/*
 * HOW MANY OF THE LAST vlhe_apply_leftover()'S RECORDS WERE FAILED -
 * the user, 2026-10-04: "There should be different wording for a
 * failed record and a power loss". An OPEN record was never undone (a
 * shutdown or power loss); a FAILED one an unload tried and could not.
 * Kept from the last call so a caller that has just asked does not
 * read the session file twice.
 */
static int g_left_failed;

int
vlhe_apply_leftover_failed(void)
{
    return g_left_failed;
}

/* A NODE VLHE MADE records the node it made ("c M,m" / "b M,m" -
 * make_nodes(), make_vmidi_node()), not what was there before - which
 * was nothing. Every other record keeps a real prior. */
static int
record_made(const struct vlhe_sess_rec *r)
{
    return r->kind == VLHE_SESS_NODE
           && (strncmp(r->prior, "c ", 2) == 0
               || strncmp(r->prior, "b ", 2) == 0);
}

int
vlhe_apply_leftover(int *mask, FILE *out)
{
    static const char *const module[3] = { "vsound", "vmidi", "vdisc" };
    static const char *const label[3]  = { "Sound", "MIDI", "CD" };
    struct vlhe_sess_rec *rec;
    int n, i, c, count = 0, nfailed = 0, m = 0, up[3], pass;

    if (mask != NULL)
        *mask = 0;
    g_left_failed = 0;
    rec = vlhe_session_read_all(&n);
    if (n <= 0)
        return 0;
    for (c = 0; c < 3; c++)
        up[c] = vlhe_status_module_loaded(module[c]);

    /*
     * LEFT OVER = IN EFFECT ON RECORD, WITH ITS COMPONENT NOT UP. A
     * loaded component's OPEN records are simply in effect (D01's
     * Load-again must keep working); a reboot or power loss clears the
     * modules and leaves the records. FAILED counts too - the machine is
     * still carrying it. Per component, so a half-failed Unload's
     * unloaded parts are found while the parts still up are not.
     *
     * TWO PASSES - the first counts, so the heading can say which kind
     * it is before the second lists them.
     */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            if (rec[i].state != VLHE_SESS_OPEN
                && rec[i].state != VLHE_SESS_FAILED)
                continue;
            c = record_component(&rec[i]);
            if (up[c])
                continue;
            if (pass == 0) {
                m |= 1 << c;
                count++;
                if (rec[i].state == VLHE_SESS_FAILED)
                    nfailed++;
                continue;
            }
            if (out == NULL)
                break;
            fprintf(out, "#     %-5s %s%s (%s %s)\n", label[c],
                    rec[i].what,
                    rec[i].state == VLHE_SESS_FAILED ? " - FAILED to undo"
                                                     : "",
                    record_made(&rec[i]) ? "made" : "was",
                    rec[i].prior[0] != '\0' ? rec[i].prior : "absent");
        }
        if (pass == 0 && count > 0 && out != NULL) {
            if (nfailed == 0)
                fprintf(out, "#   the last load was not unloaded (a shutdown"
                             " or power loss?) - still on record in %s:\n",
                        vlhe_session_path());
            else if (nfailed == count)
                fprintf(out, "#   the last unload could not undo these -"
                             " still on record in %s:\n",
                        vlhe_session_path());
            else
                fprintf(out, "#   the last load was not fully unloaded -"
                             " still on record in %s (FAILED: the unload"
                             " could not undo it; the rest were never"
                             " undone - a shutdown or power loss?):\n",
                        vlhe_session_path());
        }
        if (pass == 0 && (count == 0 || out == NULL))
            break;
    }
    free(rec);
    if (mask != NULL)
        *mask = m;
    g_left_failed = nfailed;
    return count;
}

/*
 * THE ONE-TIME NOTICE - design/54 7h decision 2, 2026-10-03. The boot
 * script's output goes to /dev/null, so anything the boot learns - a
 * leftover it finished, a CONFLICT, a FAILED that kept VLHE unloaded,
 * later the baseline's drift - is written here instead, beside the
 * session file, and shown ONCE by the Status page or `vlhe status',
 * then removed. Everything is also in the journal.
 */
static const char *
notice_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *sp = vlhe_session_path();
    const char *slash = strrchr(sp, '/');
    size_t dl = slash != NULL ? (size_t) (slash - sp) + 1 : 0;

    if (dl + sizeof "vlhe-notice" > sizeof buf)
        return NULL;
    memcpy(buf, sp, dl);
    strcpy(buf + dl, "vlhe-notice");
    return buf;
}

int
vlhe_apply_notice_add(const char *text)
{
    const char *p = notice_path();
    char stamp[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    FILE *f;

    if (p == NULL || text == NULL)
        return -1;
    f = vlhe_safe_append(p);       /* no link followed - design/55 rec. 2 */
    if (f == NULL)
        return -1;
    if (tm != NULL)
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", tm);
    else
        strcpy(stamp, "(no clock)");
    fprintf(f, "%s  %s\n", stamp, text);
    (void) fflush(f);
    (void) fsync(fileno(f));
    return fclose(f) == 0 ? 0 : -1;
}

/*
 * READ THE NOTICE WITHOUT TAKING IT - design/54 D63, 2026-10-04. The
 * GUI shows the notice in a dialog and must remove it only once shown,
 * and only the copy it showed: a line the boot appends in between must
 * not go with it. So the read records what it saw (inode, size, mtime)
 * and vlhe_apply_notice_clear() removes the file only if it is still
 * that. Returns the bytes read (0: there is no notice), or -1 when the
 * file is there but cannot be read - a setuid GUI before Modify, which
 * tries again later. The CLI keeps vlhe_apply_notice_take().
 */
int
vlhe_apply_notice_read(char *buf, size_t max, struct vlhe_notice_seen *seen)
{
    const char *p = notice_path();
    struct stat st;
    FILE *f;
    size_t n;

    if (buf == NULL || max == 0)
        return -1;
    buf[0] = '\0';
    if (p == NULL)
        return 0;
    f = vlhe_safe_state(p);
    if (f == NULL)
        return errno == ENOENT ? 0 : -1;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (seen != NULL) {
        seen->ino   = (long) st.st_ino;
        seen->size  = (long) st.st_size;
        seen->mtime = (long) st.st_mtime;
    }
    return (int) n;
}

/* 0 removed (or already gone); -1 with errno - EAGAIN when the notice
 * changed since it was read (it is left, to be shown again), anything
 * else when it could not be removed (EACCES: try again after Modify). */
int
vlhe_apply_notice_clear(const struct vlhe_notice_seen *seen)
{
    const char *p = notice_path();
    struct stat st;

    if (p == NULL || seen == NULL)
        return 0;
    if (lstat(p, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if ((long) st.st_ino != seen->ino || (long) st.st_size != seen->size
        || (long) st.st_mtime != seen->mtime) {
        errno = EAGAIN;
        return -1;
    }
    if (unlink(p) != 0)
        return errno == ENOENT ? 0 : -1;
    return 0;
}

int
vlhe_apply_notice_take(FILE *out)
{
    const char *p = notice_path();
    char line[512];
    FILE *f;
    int any = 0;

    if (p == NULL || (f = vlhe_safe_state(p)) == NULL)   /* judged first */
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (out != NULL)
            fputs(line, out);
        any = 1;
    }
    fclose(f);
    /* SHOWN, SO GONE - unless whoever looked cannot remove it (a user
     * reading root's file), in which case it is shown again until
     * someone who can has seen it. */
    (void) unlink(p);
    return any;
}

/*
 * FINISH A LOAD THAT WAS NEVER UNLOADED - design/54 7h Stage 2. The one
 * engine behind the boot finish and the Status page's "Finish the
 * unload" (Stage 2c): the unload plans of just the components left
 * over run - their restores are scoped, so a component that is up is
 * not touched, and their stop and rmmod steps find nothing - in ONE
 * journal run, under the run lock, with the same CONFLICT rule as any
 * unload; then any kernel-log capture the load left open is closed.
 * `why' is noted in the session file first, so its filed copy explains
 * itself.
 *
 * Returns how many records are STILL left over (0 = finished), or -2
 * when another Load or Unload holds the run lock (nothing was done).
 * `conflicts' and `still' (either may be NULL) get the journal's
 * CONFLICT count and its outstanding count, for the caller's summary.
 */
int
vlhe_apply_finish_leftover(const char *why, FILE *out, int *conflicts,
                           int *still)
{
    char note[256], stamp[32], who[256];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int mask, c, lk;

    if (conflicts != NULL)
        *conflicts = 0;
    if (still != NULL)
        *still = -2;
    if (vlhe_apply_leftover(&mask, out) == 0)
        return 0;

    vlhe_root_begin();
    lk = vlhe_apply_lock(who, sizeof who);
    if (lk == 1) {
        if (out != NULL)
            fprintf(out, "# refusing: another Load or Unload is running"
                         " (pid %s) - try again when it has finished\n",
                    who[0] != '\0' ? who : "unknown");
        vlhe_root_end();
        return -2;
    }

    if (why != NULL) {
        if (tm != NULL)
            strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", tm);
        else
            strcpy(stamp, "(no clock)");
        sprintf(note, "%s: %.200s", stamp, why);
        (void) vlhe_session_note(note);
    }

    (void) vlhe_journal_begin(1);
    for (c = 0; c < 3; c++) {
        struct vlhe_plan up;

        if (!(mask & (1 << c)))
            continue;
        if (vlhe_plan_build_scoped(&up, 1, c) > 0)
            (void) vlhe_plan_run(&up, out);
    }
    vlhe_apply_session_close(out);      /* what is left, said once */
    if (conflicts != NULL)
        *conflicts = vlhe_journal_conflicts();
    if (still != NULL)
        *still = vlhe_journal_outstanding_now();
    vlhe_journal_end();

    /*
     * AND THE CAPTURE THAT LOAD STARTED - 86Box 2026-10-03. A reset
     * leaves trace.pid/trace.dir behind with syslog marked stopped, and
     * the Unload button closes them but this did not: after Finish they
     * stayed on the image and the run never got its DAEMON.LOG.
     * vlhe_capture_end() is guarded on the files, signals only a live
     * `cat /proc/kmsg', and restarts syslog only if it was stopped.
     */
    (void) vlhe_capture_end(out);

    if (lk == 0)
        vlhe_apply_unlock();
    vlhe_root_end();
    return vlhe_apply_leftover(NULL, NULL);
}

/*
 * FINISH, AT BOOT, A LOAD THAT WAS NEVER UNLOADED - design/54 7h
 * decision 2 (the user: "I accept that"). Called by `vlhe apply --boot'
 * before it loads. The unload plans of just the components left over
 * run - their restores are scoped, so a component that is up is not
 * touched, and their stop and rmmod steps find nothing - under the
 * same CONFLICT rule as any unload. The session is noted as found at
 * boot, so its filed copy explains itself.
 *
 * Returns 0 nothing was left over; 1 finished - the Load may go on;
 * -1 not finished - [Boot] FinishLeftover is 0, or something is still
 * FAILED - and the Load must not start a session over it. Either way
 * the one-time notice says what happened.
 */
int
vlhe_apply_boot_finish(FILE *out)
{
    char note[640];
    const char *why;
    int left, nfailed, conflicts = 0;

    left = vlhe_apply_leftover(NULL, out);
    if (left == 0)
        return 0;

    /* FAILED IS NOT A POWER LOSS - the user, 2026-10-04. An unload that
     * could not undo something, then a reboot, leaves FAILED records;
     * only OPEN ones mean the machine went down while loaded. */
    nfailed = vlhe_apply_leftover_failed();
    why = nfailed == 0 ? "The last load was not unloaded (a shutdown or"
                         " power loss?)"
        : nfailed == left ? "The last unload could not undo everything"
        : "The last load was not fully unloaded (some changes the"
          " unload could not undo, the rest never undone - a shutdown"
          " or power loss?)";

    if (!vlhe_boot_finish_leftover()) {
        sprintf(note, "%s and %d change(s) are still on"
                      " record. [Boot] FinishLeftover is 0, so the boot"
                      " left them and did NOT load VLHE - it loads once"
                      " that unload is finished.", why, left);
        (void) vlhe_apply_notice_add(note);
        return -1;
    }

    left = vlhe_apply_finish_leftover("found at boot after an unclean"
                                      " shutdown - the boot finished this"
                                      " unload", out, &conflicts, NULL);
    if (left == -2) {
        sprintf(note, "%s, and another Load or Unload was"
                      " running at boot, so the boot did not finish it"
                      " and did NOT load VLHE.", why);
        (void) vlhe_apply_notice_add(note);
        return -1;
    }
    if (left != 0) {
        sprintf(note, "%s. The boot tried to finish it and %d"
                      " change(s) could not be put back (FAILED), so it"
                      " did NOT load VLHE. Details: %.300s.", why, left,
                vlhe_journal_path());
        (void) vlhe_apply_notice_add(note);
        return -1;
    }
    sprintf(note, "%s; the boot finished it and then loaded VLHE.%s",
            why, conflicts > 0 ? " Some paths had been changed by someone else"
                            " and were left as found (CONFLICT)." : "");
    (void) vlhe_apply_notice_add(note);
    if (conflicts > 0) {
        sprintf(note, "%d CONFLICT(s) - see %s.", conflicts,
                vlhe_journal_path());
        (void) vlhe_apply_notice_add(note);
    }
    return 1;
}

/*
 * WHICH WANTED DRIVES vdiscd HAS NOT ATTACHED. Its `status' reply names
 * each attached drive as ` N=' (vdiscd.c cmd_status: "ok a=... c=N
 * 0=T/A:path ..."), so a drive is attached when ` N=' appears. The
 * `a=' and `c=' fields come first and never have a digit before `='.
 */
int
vlhe_apply_drives_missed(const int *wanted, int ndrives, const char *reply,
                         int *missed, int max)
{
    int i, n = 0;
    char tok[16];

    if (wanted == NULL || reply == NULL || strncmp(reply, "ok", 2) != 0)
        return 0;
    for (i = 0; i < ndrives && n < max; i++) {
        if (!wanted[i])
            continue;
        sprintf(tok, " %d=", i);
        if (strstr(reply, tok) == NULL)
            missed[n++] = i;
    }
    return n;
}

int
vlhe_apply_boot_drives(FILE *out)
{
    struct vdiscd_state st;
    int wanted[VDISC_MAX_DEVS], missed[VDISC_MAX_DEVS];
    char reply[VDISCD_CTL_LINE], note[400];
    int i, any = 0, tries, n;

    if (!vlhe_drives_autoload())
        return 0;                   /* nothing was asked to come back */
    if (vdiscd_state_read(vlhe_conf_drives_path(), &st) != 0)
        return 0;
    for (i = 0; i < VDISC_MAX_DEVS; i++) {
        wanted[i] = vdiscd_state_wanted(&st, i);
        any |= wanted[i];
    }
    if (!any)
        return 0;

    /*
     * ASK, A FEW TIMES. vdiscd reattaches before it starts answering
     * its control channel, so a reply means the reattaching is over -
     * but a large .cue on a slow disk can take a moment, and a daemon
     * that is not there at all (CD not in this load) must not be
     * reported as drives that failed. Five seconds, then say nothing.
     */
    for (tries = 0; tries < 10; tries++) {
        if (vdiscd_ctl_send("status", reply, sizeof reply, 500) == 0)
            break;
        usleep(500000);
    }
    if (tries == 10)
        return 0;

    n = vlhe_apply_drives_missed(wanted, VDISC_MAX_DEVS, reply,
                                 missed, VDISC_MAX_DEVS);
    for (i = 0; i < n; i++) {
        int d = missed[i];

        sprintf(note, "Drive %d did not come back at boot: %.250s could"
                      " not be opened - its disk may not have been"
                      " mounted yet. It is still marked to come back.",
                d, st.path[d]);
        (void) vlhe_apply_notice_add(note);
        if (out != NULL)
            fprintf(out, "%s\n", note);
    }
    return n;
}

/* link_dsp_restore() for the host tests - design/54 7h Stage 1.3. */
int
vlhe_apply_restore_links(FILE *out)
{
    return restore_recorded_links(out);
}

int
vlhe_apply_remove_nodes(int want_midi, FILE *out)
{
    return remove_recorded_nodes(want_midi, out);
}

int
vlhe_apply_link_dsp_restore(const char *path, const char *saved,
                            const char *made, FILE *out)
{
    return link_dsp_restore(path, saved, made, out);
}

/* link_dsp_set() for the host tests - design/54 D24. */
int
vlhe_apply_link_dsp(const char *path, int idx, FILE *out)
{
    return link_dsp_set(path, idx, out);
}

static int g_lock_fd = -1;
static int g_lock_depth;

int
vlhe_apply_lock(char *who, size_t max)
{
    char path[VLHE_PATH_MAX];
    const char *dir = vlhe_status_rundir();
    struct flock fl;
    struct stat sb;
    int fd;

    if (who != NULL && max > 0)
        who[0] = '\0';
    if (g_lock_depth > 0) {
        g_lock_depth++;
        return 0;
    }
    if (strlen(dir) + sizeof "/apply.lock" > sizeof path)
        return -1;
    if (stat(dir, &sb) != 0)
        (void) mkdir(dir, 0755);
    strcpy(path, dir);
    strcat(path, "/apply.lock");

    /*
     * ROOT'S ALONE - design/55 R9, design/54 D51 (2026-10-04). The lock
     * was 0644, so any user could open it read-only and hold F_RDLCK,
     * and root's F_WRLCK then failed with "another Load or Unload is
     * running" - every Load, Unload, Finish and repair blocked by one
     * process anyone could start. It is made 0600 and never followed;
     * a lock left by an older build (0644, and handed to the `vlhe'
     * account by account_dirs, which no longer does) is taken back and
     * tightened by root through the descriptor. A plain file with one
     * link only - a hard link to something else is not a lock.
     */
    fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_nlink != 1) {
        close(fd);
        return -1;
    }
    if (sb.st_uid != geteuid() && geteuid() == 0)
        (void) fchown(fd, 0, 0);
    if ((sb.st_mode & 077) != 0)
        (void) fchmod(fd, 0600);
    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);

    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        int e = errno;

        if (e == EAGAIN || e == EACCES) {
            if (who != NULL && max > 0) {
                int n = (int) read(fd, who, max - 1);

                who[n > 0 ? n : 0] = '\0';
                if (n > 0 && who[n - 1] == '\n')
                    who[n - 1] = '\0';
            }
            close(fd);
            return 1;
        }
        close(fd);
        return -1;
    }

    {
        char pid[32];

        sprintf(pid, "%ld\n", (long) getpid());
        (void) ftruncate(fd, 0);
        (void) lseek(fd, 0, SEEK_SET);
        (void) !write(fd, pid, strlen(pid));
    }
    g_lock_fd    = fd;
    g_lock_depth = 1;
    return 0;
}

void
vlhe_apply_unlock(void)
{
    if (g_lock_depth <= 0)
        return;
    if (--g_lock_depth > 0)
        return;
    if (g_lock_fd >= 0) {
        close(g_lock_fd);       /* drops the lock */
        g_lock_fd = -1;
    }
}

/*
 * THE BASELINE, COMPARED AT EVERY LOAD - design/54 7h Stage 3.3.
 *
 * Every baseline path that no session record holds in effect (OPEN or
 * FAILED - those are VLHE's live changes, D01's Load-again) is compared
 * with what is there now:
 *
 *  - the same: nothing;
 *  - VLHE's own leftover (vlhe_apply_baseline_infer() says so - a link
 *    a session file says VLHE made, its inner /dev/cdrom link, one of
 *    its nodes): put back to the baseline and journalled - unless the
 *    baseline is UNKNOWN, which is reported and left (never guessed);
 *  - anything else: DRIFT - someone changed it since. Listed was/now,
 *    and ONE decision for the list: Accept changes (the baseline takes
 *    what is there; the old file kept, stamped), Undo changes (each put
 *    back to the baseline, then the Load - added 2026-10-04, the user:
 *    there was no way back but by hand), Load anyway (left alone, asked
 *    again next time), or Cancel. Asked BEFORE anything is restored, so
 *    a cancel changes nothing at all. Never updated silently.
 *
 * WHO DECIDES is a callback: the CLI's `[y/N/abort]' prompt on a
 * terminal, the GUI's dialog. With none - the boot, or no terminal - it
 * warns and keeps the baseline, and the one-time notice says so (7h
 * decision 2, "warn-and-keep at boot otherwise warns nobody").
 */
static int (*g_drift_ask)(const struct vlhe_drift *d, int n);

/* SET WHEN THE LAST CHECK REFUSED THE LOAD BY [Load] BaselineDrift,
 * rather than someone pressing Cancel - so the front ends can say
 * which (vlhe_apply_drift_refused()). */
static int g_drift_refused;

int
vlhe_apply_drift_refused(void)
{
    return g_drift_refused;
}

/*
 * WHICH FRONT END IS SHOWING THIS - the user, 2026-10-04: "The gui is
 * open we should not tell users to use the cli and vice versa." The
 * messages written here name only the front end running them: the
 * control centre's buttons in the GUI, the `vlhe' commands on a
 * terminal, and nothing at boot, when no one is reading (0, the
 * default - the one-time notices are worded for that).
 */
static int g_front;

void
vlhe_apply_set_frontend(int which)
{
    g_front = which;
}

/* How to finish a leftover / put a finding right, in this front end's
 * words - "" when there is no front end. */
static const char *
how_finish(void)
{
    return g_front == VLHE_FRONT_GUI ? " - Finish the unload, at the top of"
                                       " the Status page"
         : g_front == VLHE_FRONT_CLI ? " - `vlhe apply -u' finishes it"
         : "";
}

static const char *
how_fix(void)
{
    return g_front == VLHE_FRONT_GUI ? " Review... on the Status page puts"
                                       " it right."
         : g_front == VLHE_FRONT_CLI ? " `vlhe repair' puts it right."
         : "";
}

void
vlhe_apply_set_drift_ask(int (*ask)(const struct vlhe_drift *d, int n))
{
    g_drift_ask = ask;
}

/* Put `path' back to a baseline state. 0, or -1 with the reason said. */
static int
restore_to_state(const char *path, const char *state, FILE *out)
{
    struct stat sb;
    int maj, min;

    /* ONLY WHAT VLHE MAKES - recorded_ok(). The baseline sits where the
     * `vlhe' account (or a portable folder's owner) could write it. */
    if (!recorded_ok(path, state, out))
        return -1;

    if (strncmp(state, "link ", 5) == 0)
        return relink_atomic(path, state + 5, out);
    if (lstat(path, &sb) == 0 && unlink(path) != 0) {
        if (out != NULL)
            fprintf(out, "#   %s: cannot remove - %s\n", path,
                    strerror(errno));
        return -1;
    }
    if (strcmp(state, "absent") == 0)
        return 0;
    if (sscanf(state, "node %d %d", &maj, &min) == 2) {
        if (remake_node(path, 0, maj, min, state) != 0)
            goto fail;
        return 0;
    }
    if (sscanf(state, "block %d %d", &maj, &min) == 2) {
        if (remake_node(path, 1, maj, min, state) != 0)
            goto fail;
        return 0;
    }
    if (out != NULL)
        fprintf(out, "#   %s: the baseline says \"%s\" - not a state VLHE"
                     " can put back\n", path, state);
    return -1;
fail:
    if (out != NULL)
        fprintf(out, "#   %s: cannot put back %s - %s\n", path, state,
                strerror(errno));
    return -1;
}

static int
path_in_effect(const struct vlhe_sess_rec *rec, int n, const char *path)
{
    int i;

    for (i = 0; i < n; i++)
        if (rec_pending(&rec[i]) && strcmp(rec[i].what, path) == 0)
            return 1;
    return 0;
}

int
vlhe_apply_baseline_check(FILE *out)
{
    struct vlhe_base_ent *base;
    struct vlhe_sess_rec *rec;
    struct vlhe_drift *drift;
    int  nb, ns, i, nd = 0, *ours, choice = VLHE_DRIFT_KEEP, cancel = 0;
    char now[VLHE_PATH_MAX], orig[VLHE_PATH_MAX];

    g_drift_refused = 0;
    base = vlhe_baseline_read_all(&nb);
    if (nb < 0) {
        if (out != NULL)
            fprintf(out, "#   %s: cannot read - not compared\n",
                    vlhe_baseline_path());
        return 0;
    }
    if (nb == 0)
        return 0;               /* nothing recorded yet: a first Load */
    rec = vlhe_session_read_all(&ns);
    if (ns < 0)
        ns = 0;
    drift = (struct vlhe_drift *) malloc(nb * sizeof *drift);
    ours  = (int *) malloc(nb * sizeof *ours);
    if (drift == NULL || ours == NULL) {
        free(drift); free(ours); free(base); free(rec);
        return 0;
    }

    /* FIRST, ONLY LOOK. */
    for (i = 0; i < nb; i++) {
        ours[i] = 0;
        if (path_in_effect(rec, ns, base[i].what))
            continue;
        vlhe_baseline_describe(base[i].what, now, sizeof now);
        if (vlhe_state_same(now, base[i].state))
            continue;
        if (vlhe_apply_baseline_infer(base[i].what, now, orig, sizeof orig)
            != VLHE_BASE_FOUND) {
            ours[i] = 1;        /* VLHE's own leftover */
            continue;
        }
        strcpy(drift[nd].what, base[i].what);
        strcpy(drift[nd].was, base[i].how == VLHE_BASE_UNKNOWN
                              ? "unknown" : base[i].state);
        strcpy(drift[nd].now, now);
        nd++;
    }

    /* THEN ONE DECISION FOR THE DRIFT, before anything changes. */
    if (nd > 0) {
        if (out != NULL) {
            fprintf(out, "#   %d path(s) differ from the baseline in %s"
                         " - changed since VLHE first saw them:\n", nd,
                    vlhe_baseline_path());
            for (i = 0; i < nd; i++)
                fprintf(out, "#     %s: was %s, now %s\n", drift[i].what,
                        drift[i].was, drift[i].now);
            fflush(out);
        }
        /* [Load] BaselineDrift DECIDES WHETHER ANYONE IS ASKED - design/54
         * 7h Stage 4. Only with someone to ask (the GUI, a terminal):
         * the boot has no callback and always warns, whatever it says. */
        if (g_drift_ask != NULL
            && vlhe_baseline_drift() == VLHE_BDRIFT_REFUSE) {
            if (out != NULL)
                fprintf(out, "# refused - the load did not start; nothing"
                             " was changed. System Baseline is set to"
                             " Refuse to Load. The full account is in"
                             " %s\n", vlhe_journal_path());
            choice = VLHE_DRIFT_CANCEL;
            g_drift_refused = 1;
        } else if (g_drift_ask != NULL
                   && vlhe_baseline_drift() == VLHE_BDRIFT_WARN) {
            if (out != NULL)
                fprintf(out, "#   System Baseline is set to Warn and"
                             " Load:\n");
            choice = VLHE_DRIFT_KEEP;
        } else if (g_drift_ask != NULL)
            choice = g_drift_ask(drift, nd);
        else {
            char note[256];

            choice = VLHE_DRIFT_KEEP;
            sprintf(note, "%d path(s) differ from the baseline (first: %.60s,"
                          " now %.60s) - the baseline was kept as it was.",
                    nd, drift[0].what, drift[0].now);
            (void) vlhe_apply_notice_add(note);
        }
        if (choice == VLHE_DRIFT_CANCEL) {
            if (out != NULL && !g_drift_refused)
                fprintf(out, "# cancelled - the load did not start;"
                             " nothing was changed\n");
            cancel = 1;
        } else if (choice == VLHE_DRIFT_UPDATE) {
            for (i = 0; i < nd; i++)
                if (vlhe_baseline_update(drift[i].what, drift[i].now,
                                         VLHE_BASE_FOUND) != 0
                    && out != NULL)
                    fprintf(out, "#   cannot update %s for %s\n",
                            vlhe_baseline_path(), drift[i].what);
            if (out != NULL)
                fprintf(out, "#   baseline updated - the old one is kept"
                             " beside it\n");
        } else if (choice == VLHE_DRIFT_UNDO) {
            /* PUT BACK AS THE BASELINE HAS THEM, before the Load - an
             * UNKNOWN original is never guessed (reported and left). */
            for (i = 0; i < nd; i++) {
                struct vlhe_base_ent e;
                char det[VLHE_PATH_MAX + 64];

                if (vlhe_baseline_get(drift[i].what, &e) != 1
                    || e.how == VLHE_BASE_UNKNOWN) {
                    if (out != NULL)
                        fprintf(out, "#   %s: what it was is not known -"
                                     " left as it is\n", drift[i].what);
                    continue;
                }
                if (out != NULL) {
                    shell_restore(out, drift[i].what,
                                  strcmp(e.state, "absent") == 0
                                      ? NULL : e.state);
                    fflush(out);
                }
                if (restore_to_state(drift[i].what, e.state, out) == 0) {
                    if (out != NULL)
                        fprintf(out, "#   %s put back to %s, as the"
                                     " baseline has it\n", drift[i].what,
                                e.state);
                    sprintf(det, "back to %.200s (undone by choice - it"
                                 " was %.100s)", e.state, drift[i].now);
                    vlhe_journal_add(VLHE_CH_RMNODE, drift[i].what, det);
                } else {
                    vlhe_journal_failed(VLHE_CH_RMNODE, drift[i].what,
                                        "not put back to the baseline");
                }
            }
        } else if (out != NULL) {
            fprintf(out, "#   left as it is - the baseline unchanged\n");
        }
    }

    /* AND VLHE'S OWN LEFTOVERS PUT BACK - only if the Load goes on. */
    for (i = 0; !cancel && i < nb; i++) {
        if (!ours[i])
            continue;
        vlhe_baseline_describe(base[i].what, now, sizeof now);
        if (base[i].how == VLHE_BASE_UNKNOWN) {
            if (out != NULL)
                /* NOT "BY HAND" - the Needs attention row and `vlhe
                 * repair' put it right, so the note names this front
                 * end's way (2026-10-04, as the DANGLING note does). */
                fprintf(out, "#   %s is VLHE's leftover (%s) and what it"
                             " was before is not known - left as it"
                             " is.%s\n", base[i].what, now, how_fix());
            continue;
        }
        if (out != NULL) {
            shell_restore(out, base[i].what,
                          strncmp(base[i].state, "absent", 6) == 0
                              ? NULL : base[i].state);
            fflush(out);
        }
        if (restore_to_state(base[i].what, base[i].state, out) == 0) {
            char det[VLHE_PATH_MAX + 64];

            if (out != NULL)
                fprintf(out, "#   %s was VLHE's leftover (%s) - put back"
                             " to %s, as the baseline has it\n",
                        base[i].what, now, base[i].state);
            sprintf(det, "back to %.200s (VLHE's leftover, from the"
                         " baseline)", base[i].state);
            vlhe_journal_add(VLHE_CH_RMNODE, base[i].what, det);
        } else {
            vlhe_journal_failed(VLHE_CH_RMNODE, base[i].what,
                                "leftover not put back to the baseline");
        }
    }

    free(drift); free(ours); free(base); free(rec);
    return cancel ? -1 : 0;
}

/*
 * WHAT NEEDS THE USER'S ATTENTION - design/54 7h Stage 3.5. Two kinds,
 * both of them things VLHE may have left and cannot prove it did:
 *
 *  - a /dev/dsp-style link, when VLHE holds no record of it in effect:
 *    marked DANGLING in the baseline (3.5's seed), or - with no baseline
 *    entry and no session record anywhere, the folder lost - a link to
 *    /dev/dspN at which nothing answers. Offered: the stock node, or
 *    keep it. NOT a refusal - it may be the user's own link to a card
 *    whose driver is not loaded yet;
 *  - /dev/cdrom pointing at VLHE's inner link or a vdisc node that is
 *    gone, with vdisc not loaded - certainly VLHE's, original unknown.
 *    Offered: point it at a drive, or remove it. Never while CD is
 *    loaded: then the link is VLHE's working redirect.
 */
struct count_ctx { int n; };
static void
count_visit(const struct vlhe_sess_rec *r, void *vctx)
{
    (void) r;
    ((struct count_ctx *) vctx)->n++;
}

static int
add_finding(struct vlhe_finding *f, int n, int max, int kind,
            const char *path, const char *now, const char *text)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(f[i].path, path) == 0)
            return n;           /* once per path */
    if (n >= max)
        return n;
    f[n].kind = kind;
    f[n].was[0] = '\0';
    strncpy(f[n].path, path, sizeof f[n].path - 1);
    f[n].path[sizeof f[n].path - 1] = '\0';
    strncpy(f[n].now, now, sizeof f[n].now - 1);
    f[n].now[sizeof f[n].now - 1] = '\0';
    strncpy(f[n].text, text, sizeof f[n].text - 1);
    f[n].text[sizeof f[n].text - 1] = '\0';
    return n + 1;
}

int
vlhe_apply_findings(struct vlhe_finding *f, int max)
{
    struct vlhe_base_ent *base;
    struct vlhe_sess_rec *rec;
    struct vlhe_sound so;
    char now[VLHE_PATH_MAX], text[2 * VLHE_PATH_MAX + 128];
    const char *cand[2];
    int  nb, ns, i, n = 0;

    rec = vlhe_session_read_all(&ns);
    if (ns < 0)
        ns = 0;

    /* MARKED IN THE BASELINE */
    base = vlhe_baseline_read_all(&nb);
    for (i = 0; i < nb; i++) {
        if (base[i].how != VLHE_BASE_DANGLING
            || path_in_effect(rec, ns, base[i].what))
            continue;
        vlhe_baseline_describe(base[i].what, now, sizeof now);
        sprintf(text, "%.200s is %.200s - it may be an earlier VLHE load's"
                      " leftover, or your own setup", base[i].what, now);
        n = add_finding(f, n, max, VLHE_FIND_DSP, base[i].what, now, text);
    }

    /* NO BASELINE, NO RECORD: THE FOLDER LOST - a dangling dsp link */
    cand[0] = "/dev/dsp";
    cand[1] = (vlhe_sound(&so) == 0 && so.programs_use[0] != '\0')
              ? so.programs_use : NULL;
    for (i = 0; i < 2; i++) {
        struct count_ctx c;

        if (cand[i] == NULL || vlhe_baseline_get(cand[i], NULL) != 0
            || path_in_effect(rec, ns, cand[i]))
            continue;
        vlhe_baseline_describe(cand[i], now, sizeof now);
        if (strncmp(now, "link ", 5) != 0
            || strncmp(now + 5, "/dev/dsp", 8) != 0
            || dsp_target_dangling(now + 5) != 1)
            continue;
        c.n = 0;
        session_files_each(cand[i], count_visit, &c);
        if (c.n != 0)
            continue;           /* on record: the Load's check handles it */
        sprintf(text, "%.200s points to %.200s, and nothing answers there -"
                      " it may be an earlier VLHE load's leftover, or your"
                      " own setup", cand[i], now + 5);
        n = add_finding(f, n, max, VLHE_FIND_DSP, cand[i], now, text);
    }

    /* /dev/cdrom AT VLHE'S OWN TARGET, GONE, WITH CD NOT LOADED */
    if (!vlhe_status_module_loaded("vdisc")) {
        vlhe_baseline_describe("/dev/cdrom", now, sizeof now);
        if (strncmp(now, "link ", 5) == 0 && target_is_cdrom_ours(now + 5)) {
            struct stat sb;

            if (stat(now + 5, &sb) != 0) {
                struct vlhe_base_ent e;
                const char *was = NULL;

                /* WHAT THE BASELINE KNOWS IT WAS, said - 2026-10-04, the
                 * user's part-4 run offered Point at drive with no word
                 * of /dev/hdb, which the baseline had. Only a link the
                 * baseline found or inferred; UNKNOWN is never guessed. */
                if (vlhe_baseline_get("/dev/cdrom", &e) == 1
                    && e.how != VLHE_BASE_UNKNOWN
                    && strncmp(e.state, "link ", 5) == 0)
                    was = e.state + 5;
                if (was != NULL)
                    sprintf(text, "/dev/cdrom points to %.200s, which no"
                                  " longer exists - it was left by an"
                                  " earlier VLHE load; before that it"
                                  " pointed to %.200s", now + 5, was);
                else
                    sprintf(text, "/dev/cdrom points to %.200s, which no"
                                  " longer exists - it was left by an"
                                  " earlier VLHE load", now + 5);
                n = add_finding(f, n, max, VLHE_FIND_CDROM, "/dev/cdrom",
                                now, text);
                if (n > 0 && strcmp(f[n - 1].path, "/dev/cdrom") == 0) {
                    strncpy(f[n - 1].was, was != NULL ? was : "",
                            sizeof f[n - 1].was - 1);
                    f[n - 1].was[sizeof f[n - 1].was - 1] = '\0';
                }
            }
        }
    }

    free(base);
    free(rec);
    return n;
}

/*
 * PUTTING A FINDING RIGHT - design/54 7h Stage 3.5. The user's choice, from
 * `vlhe repair' or the Status page, never automatic. Whatever is chosen
 * is written into the baseline as FOUND, which clears the finding. The
 * choices that change /dev pass the same machine check as `vlhe apply',
 * run raised and under the run lock; Keep only rewrites the baseline.
 * Each re-checks that the finding still holds first.
 */
static int
finding_still(const char *path, int kind, struct vlhe_finding *fo)
{
    struct vlhe_finding f[8];
    int n = vlhe_apply_findings(f, 8), i;

    for (i = 0; i < n; i++)
        if (f[i].kind == kind && strcmp(f[i].path, path) == 0) {
            if (fo != NULL)
                *fo = f[i];
            return 1;
        }
    return 0;
}

static int
baseline_set(const char *path, const char *state)
{
    return vlhe_baseline_get(path, NULL) > 0
           ? vlhe_baseline_update(path, state, VLHE_BASE_FOUND)
           : (vlhe_baseline_add(path, state, VLHE_BASE_FOUND) >= 0 ? 0 : -1);
}

/* MAKEDEV's node for a dsp name: /dev/dsp 14,3, /dev/dspN 14,16N+3. */
static int
stock_dsp_state(const char *path, char *out, size_t max)
{
    int n;
    char tail;

    if (strcmp(path, "/dev/dsp") == 0)
        n = 0;
    else if (sscanf(path, "/dev/dsp%d%c", &n, &tail) != 1 || n < 0 || n > 15)
        return -1;
    if (max < 16)
        return -1;
    sprintf(out, "node 14 %d", 16 * n + 3);
    return 0;
}

int
vlhe_apply_fix_finding(const char *path, int kind, int action,
                       const char *arg, FILE *out)
{
    struct vlhe_finding f;
    char st[VLHE_PATH_MAX + 8], why[256], det[VLHE_PATH_MAX + 64];
    int  rc = -1, lk;

    st[0] = '\0';
    if (!finding_still(path, kind, &f)) {
        if (out != NULL)
            fprintf(out, "%s no longer needs attention - nothing done\n",
                    path);
        return 0;
    }

    /* KEEP IS LOCKED AND RAISED LIKE THE REST - design/55 R14, design/54
     * D56 (2026-10-04). It wrote the baseline here, before any check,
     * without the run lock and without raising, so a keep could land in
     * the middle of someone else's Load. */
    if (action == VLHE_FIX_KEEP && kind != VLHE_FIND_DSP)
        return -1;              /* a link into VLHE's run dir never works */

    /* THE MACHINE CHECK GUARDS /dev, WHICH KEEP DOES NOT TOUCH - it
     * writes only the baseline - so keep skips it and still takes the
     * lock and the raise below. */
    if (action != VLHE_FIX_KEEP && vlhe_apply_machine_ok(why, sizeof why) != 0) {
        if (out != NULL)
            fprintf(out, "refusing: %s\n", why);
        return -1;
    }
    vlhe_root_begin();
    lk = vlhe_apply_lock(why, sizeof why);
    if (lk == 1) {
        if (out != NULL)
            fprintf(out, "refusing: another Load or Unload is running (pid"
                         " %s)\n", why);
        vlhe_root_end();
        return -1;
    }
    (void) vlhe_journal_begin(VLHE_JOURNAL_REPAIR);

    if (action == VLHE_FIX_KEEP) {
        rc = baseline_set(path, f.now);
        if (out != NULL)
            fprintf(out, rc == 0 ? "%s kept as it is - the baseline now has"
                                   " it as found\n"
                                 : "%s: cannot write %s\n", path,
                    rc == 0 ? "" : vlhe_baseline_path());
        if (rc == 0) {
            sprintf(det, "%.200s (kept as found)", f.now);
            vlhe_journal_add(VLHE_CH_NODE, path, det);
        }
        vlhe_journal_end();
        if (lk == 0)
            vlhe_apply_unlock();
        vlhe_root_end();
        return rc;
    }

    if (action == VLHE_FIX_STOCK && kind == VLHE_FIND_DSP
        && stock_dsp_state(path, st, sizeof st) == 0) {
        rc = restore_to_state(path, st, out);
    } else if (action == VLHE_FIX_DRIVE && kind == VLHE_FIND_CDROM
               && arg != NULL) {
        struct stat sb;

        if (stat(arg, &sb) != 0 || !S_ISBLK(sb.st_mode)) {
            if (out != NULL)
                fprintf(out, "%s is not a block device - /dev/cdrom not"
                             " changed\n", arg);
        } else {
            sprintf(st, "link %.*s", VLHE_PATH_MAX - 8, arg);
            rc = relink_atomic("/dev/cdrom", arg, out);
        }
    } else if (action == VLHE_FIX_REMOVE && kind == VLHE_FIND_CDROM) {
        strcpy(st, "absent");
        rc = restore_to_state("/dev/cdrom", "absent", out);
    } else if (out != NULL) {
        fprintf(out, "that choice does not apply to %s\n", path);
    }

    if (rc == 0) {
        if (baseline_set(path, st) != 0 && out != NULL)
            fprintf(out, "#   cannot write %s\n", vlhe_baseline_path());
        sprintf(det, "%.200s (put right by choice - was %.100s)", st, f.now);
        vlhe_journal_add(VLHE_CH_NODE, path, det);
        if (out != NULL)
            fprintf(out, "%s is now %s\n", path, st);
    } else if (st[0] != '\0') {
        vlhe_journal_failed(VLHE_CH_NODE, path, "not put right");
    }
    vlhe_journal_end();
    if (lk == 0)
        vlhe_apply_unlock();
    vlhe_root_end();
    return rc;
}

/* THE MACHINE'S CD DRIVES, for Point at drive: IDE units whose
 * /proc/ide/hdX/media says cdrom, and SCSI CD-ROMs from /proc/scsi/scsi
 * as /dev/scdN in their order. VLHE_PROC_IDE / VLHE_PROC_SCSI move the
 * two for the host tests. */
int
vlhe_apply_cd_drives(char (*list)[VLHE_PATH_MAX], int max)
{
    const char *ide = getenv("VLHE_PROC_IDE");
    const char *scsi = getenv("VLHE_PROC_SCSI");
    char p[VLHE_PATH_MAX + 32], line[256];
    int n = 0, u, scd = 0;
    FILE *f;

    if (ide == NULL || *ide == '\0')
        ide = "/proc/ide";
    if (scsi == NULL || *scsi == '\0')
        scsi = "/proc/scsi/scsi";
    for (u = 0; u < 8 && n < max; u++) {
        sprintf(p, "%.*s/hd%c/media", VLHE_PATH_MAX - 16, ide, 'a' + u);
        f = fopen(p, "r");
        if (f == NULL)
            continue;
        if (fgets(line, sizeof line, f) != NULL
            && strncmp(line, "cdrom", 5) == 0)
            sprintf(list[n++], "/dev/hd%c", 'a' + u);
        fclose(f);
    }
    f = fopen(scsi, "r");
    if (f != NULL) {
        while (n < max && fgets(line, sizeof line, f) != NULL)
            if (strstr(line, "Type:") != NULL && strstr(line, "CD-ROM") != NULL)
                sprintf(list[n++], "/dev/scd%d", scd++);
        fclose(f);
    }
    return n;
}

/* The findings as `#' lines - the Load names them and carries on. */
static void
print_findings(FILE *out)
{
    struct vlhe_finding f[8];
    int n, i;

    if (out == NULL)
        return;
    n = vlhe_apply_findings(f, 8);
    for (i = 0; i < n; i++)
        fprintf(out, "#   NOTE: %s.%s\n", f[i].text, how_fix());
}

int
vlhe_plan_run(const struct vlhe_plan *p, FILE *out)
{
    int  rc, lk;
    char who[256];      /* the lock holder's pid, or the refusal */

    /* THE MACHINE CHECK BEFORE THE LOCK - a run refused as not a
     * 2.2.16 machine must touch nothing, the run directory included.
     * plan_run_raised() checks again; that is the guard proper. */
    if (p != NULL && vlhe_apply_machine_ok(who, sizeof who) != 0) {
        if (out != NULL)
            fprintf(out, "# refusing: %s\n", who);
        return -1;
    }

    vlhe_root_begin();
    lk = vlhe_apply_lock(who, sizeof who);
    /*
     * NO NEW SESSION ON TOP OF AN UNFINISHED ONE - design/54 7h Stage 2.
     * A load that finds records left over by a load that was never
     * unloaded refuses, lists them, and says how to finish: what it
     * would change is what those records say must be put back first.
     * After the lock, so the answer cannot change under it.
     */
    if (lk != 1 && p != NULL && !p->unload) {
        int mask;

        if (vlhe_apply_leftover(&mask, out) > 0) {
            if (out != NULL)
                fprintf(out, "# refusing: finish that unload first%s."
                             " Nothing was changed.\n", how_finish());
            if (lk == 0)
                vlhe_apply_unlock();
            vlhe_root_end();
            return -1;
        }
    }
    if (lk == 1) {
        if (out != NULL)
            fprintf(out, "# refusing: another Load or Unload is running"
                         " (pid %s) - try again when it has finished\n",
                    who[0] != '\0' ? who : "unknown");
        vlhe_root_end();
        return -1;
    }
    if (lk < 0 && out != NULL)
        fprintf(out, "#   (could not take the run lock in %s - %s;"
                     " going on without it)\n",
                vlhe_status_rundir(), strerror(errno));
    rc = plan_run_raised(p, out);
    if (lk == 0)
        vlhe_apply_unlock();
    vlhe_root_end();
    return rc;
}

static int
plan_run_raised(const struct vlhe_plan *p, FILE *out)
{
    char why[256];
    int  i;
    int  last_comp = -1;        /* for shell_heading() */
    char cmdbuf[VLHE_CMD_MAX];  /* the expanded command, printed then run */

    if (p == NULL)
        return -1;

    /*
     * THE MACHINE CHECK AGAIN, even though every caller is supposed
     * to have run it. This is the function that loads modules built
     * for another kernel; a guard that depends on the caller
     * remembering is not a guard, and the cost is two file reads.
     */
    if (vlhe_apply_machine_ok(why, sizeof why) != 0) {
        if (out != NULL)
            fprintf(out, "# refusing: %s\n", why);
        return -1;
    }

    /*
     * A TRUNCATED PLAN IS NOT RUN - design/47 T3. add() marks the plan
     * when VLHE_PLAN_MAX is reached and drops the step; only the
     * printer ever said so, and this ran the first 32 steps as if
     * they were the whole - a load with its last daemon or node
     * missing, or an unload that stopped before the restore steps,
     * with nothing on the Status page to say which. Out of reach of
     * today's plans (a full load is well under the cap) and refused
     * anyway, because the day it is reached the plan is wrong.
     */
    if (p->truncated) {
        if (out != NULL)
            fprintf(out, "# refusing: the plan has more than %d steps and"
                         " was cut short - this is a bug, please report"
                         " it\n", VLHE_PLAN_MAX);
        return -1;
    }

    /*
     * AND THE CONFIG THIS PLAN WAS BUILT FROM - design/49 T0. Every
     * choice in it - which modules, their options, which daemons -
     * came from the system file, and this function acts on them as
     * root. If a user other than root could have written that file,
     * nothing is done. At boot the init script discards the output
     * and goes on, so this fails the apply and never the boot.
     *
     * LOAD ONLY - design/49 R6, after a target run was left with the
     * modules loaded and no way to remove them: root's own Save had
     * made a group-writable file (fixed in vlhe_commit()), and the
     * unload refused on it too. An unload takes its nodes and links
     * from the session record, and whatever it still reads from the
     * config it now reads as THE DEFAULTS, because load_config()
     * keeps an untrusted file's values out of memory (R1). So an
     * untrusted file cannot steer an unload, and refusing one only
     * strands the machine. A load on defaults while the user's file
     * sits ignored WOULD be a silent wrong result, so that refusal
     * stays.
     */
    if (!p->unload && !vlhe_conf_system_trusted(why, (int) sizeof why)) {
        /*
         * NOT "chown root" - that accepts every value in the file
         * unchecked, which is what this refusal exists to stop, and in
         * a directory others can write it would not pass anyway. The
         * supported way is Load Configuration, which checks each value
         * and saves a file of root's own; a directory named here has
         * to be one only root can write.
         */
        if (out != NULL)
            fprintf(out, "# refusing: %s.\n%s"
                         " If a directory is named, the configuration"
                         " must live where only root can write.\n", why,
                    g_front == VLHE_FRONT_CLI
                    ? "  The configuration must be root's own: `vlhe setup',"
                      " as root, writes one."
                    : "  As root, use File > Load Configuration to import"
                      " it - that checks every value and saves root's own"
                      " copy.");
        return -1;
    }

    /*
     * THE CHANGE JOURNAL. Opened AFTER the machine check, so a run
     * that refuses to do anything does not write a header saying it
     * did.
     *
     * A FAILURE HERE DOES NOT STOP THE RUN - refusing to load modules
     * because a log file is unwritable would be the tail wagging the
     * dog. It is reported and the run proceeds unrecorded, which the
     * user can see.
     */
    if (vlhe_journal_begin(p->unload) != 0 && out != NULL)
        fprintf(out, "# note: cannot write %s - changes will not be"
                     " journalled (%s)\n",
                vlhe_journal_path(), strerror(errno));

    /* THE BASELINE, BEFORE THE FIRST CHANGE - design/54 7h Stage 3.3.
     * Not when nested: the GUI's press checks once, before its scopes. */
    if (!p->unload && !vlhe_journal_nested()
        && vlhe_apply_baseline_check(out) != 0) {
        vlhe_journal_end();
        return -1;
    }
    /* AND WHAT NEEDS ATTENTION, NAMED - the Load carries on (7h 3.5). */
    if (!p->unload && !vlhe_journal_nested())
        print_findings(out);

    for (i = 0; i < p->n; i++) {
        const struct vlhe_step *st = &p->step[i];
        int rc;
        int pre_loaded = 0;     /* was this module resident already? */

        /*
         * A CLEANUP STEP IS NOT PART OF THE FORWARD PATH, AND THIS
         * LOOP RAN IT ANYWAY UNTIL 2026-09-24.
         *
         * WHAT IT LOOKED LIKE, from the journal of a SUCCESSFUL load:
         *
         *     create node   /dev/cdrom  was /dev/hdb
         *     start  daemon vdiscd
         *     remove node   /dev/cdrom  back to /dev/hdb
         *
         * All three inside ONE apply. The redirect the user asked for
         * survived exactly one step, so `/dev/cdrom' pointed back at
         * the real drive before they could use it and vdisc was
         * unreachable without naming /dev/vdisc0 by hand - which is
         * the one thing the option exists to avoid. design/36 row 51.
         *
         * THE FLAG WAS RIGHT AND ONLY ONE READER HONOURED IT.
         * plan_load() appends "(cdromrestore)" and calls
         * mark_cleanup(), which sets step.cleanup = 1; run_cleanup()
         * tests that flag and runs ONLY those steps. This loop never
         * tested it, so a cleanup step sitting at the end of a plan
         * that SUCCEEDS was executed as an ordinary step - undoing,
         * on the success path, the thing the plan had just done.
         *
         * `cleanup' AND `optional' ARE DIFFERENT AXES and it is worth
         * saying so here, because the names are close: `optional'
         * means "a failure here does not stop the run", and is about
         * the forward path; `cleanup' means "not on the forward path
         * at all". A step can be both.
         *
         * THE UNLOAD PATH IS UNAFFECTED - its own "(cdromrestore)" is
         * an ordinary step (plan_unload(), around line 1481) because
         * there the restore IS the work.
         *
         * AND ONLY THE PSEUDO-STEPS ARE SKIPPED, WHICH IS THE WHOLE
         * CARE IN THIS FIX. The card reload - `modprobe sb ...' - is
         * ALSO marked cleanup, and it must run on BOTH paths: we
         * removed the card three steps earlier and a successful load
         * has to put it back too. Skipping every cleanup step here
         * would leave a working machine with no sound card, which is
         * a worse bug than the one being fixed.
         *
         * So the test is step_is_note(): a parenthesised pseudo-step
         * is a thing only run_cleanup() knows how to undo, where a
         * real command marked cleanup is one the forward path still
         * needs. run_cleanup() skips notes today for a different
         * reason (execvp cannot run them) and now has a branch for
         * this one.
         */
        if (st->cleanup && step_is_note(st->cmd))
            continue;

        /*
         * `modprobe soundcore' ONCE PER PRESS - 2026-10-04, the user's
         * part-5 Load box: every scope adds it (vsound and vmidi both
         * import from it, and a CD-only trial may need it first), so
         * one press printed "#---- vsound ---- / modprobe soundcore /
         * already loaded - nothing to do" three times, twice under a
         * heading for a component that was not running. A later scope
         * of the same press, finding it resident, says nothing at all.
         */
        if (strcmp(st->cmd, "modprobe soundcore") == 0) {
            static int said_in_run = -1;
            char why[32];
            int  run = vlhe_journal_run_id();

            if (run == said_in_run
                && vlhe_apply_step_satisfied(st, why, sizeof why))
                continue;
            said_in_run = run;
        }

        /* THE COMPONENT HEADING, as the simulation prints it. */
        shell_heading(out, st->comp, &last_comp);

        /*
         * A STOP STEP IS NOT A COMMAND. It carries "(stop vsoundd)"
         * - parenthesised like a note, because that is what it was
         * until 2026-09-21 - and the runner now ACTS on it rather
         * than skipping it. The parentheses stay so a dry run still
         * reads as prose and so nothing ever hands it to execvp.
         */
        if (strncmp(st->cmd, "(stop ", 6) == 0) {
            char who[32];
            int  k;

            for (k = 0; k < (int) sizeof who - 1; k++) {
                char c = st->cmd[6 + k];
                if (c == ')' || c == '\0')
                    break;
                who[k] = c;
            }
            who[k] = '\0';

            /*
             * WAS IT RUNNING? Asked BEFORE the stop, because
             * stop_daemon() returns 0 both for "stopped it" and for
             * "there was nothing to stop" - and a journal must not
             * claim a change that did not happen. Checking after
             * would be too late; checking the return value cannot
             * tell the two apart.
             *
             * AND THE `kill' LINE IS PRINTED ONLY WHEN IT WILL RUN -
             * 86Box 2026-10-03, Finish the unload printing a kill for
             * each daemon with "no pid file" beside it. The same rule
             * as the restore command (design/54 7h).
             */
            {
                int was = vlhe_status_daemon(
                              vlhe_status_pidfile_path(who, -1)) > 0;
                int src;

                if (was && out != NULL) {
                    shell_stop(out, who);
                    fflush(out);
                }
                src = stop_daemon(who, out);

                if (src == 0) {
                    if (was)
                        vlhe_journal_add(VLHE_CH_DAEMON_STOP, who, NULL);
                } else {
                    /*
                     * "did not stop WITHIN 5s" RATHER THAN "did not
                     * stop", because the shorter wording was WRONG on
                     * 86Box 2026-09-23: all three daemons had stopped
                     * and the journal said they had not (they were
                     * unreaped zombies - see stop_daemon()). The
                     * message now says what was actually observed,
                     * which is a timeout, rather than asserting a
                     * state this code cannot confirm.
                     */
                    vlhe_journal_failed(VLHE_CH_DAEMON_STOP, who,
                                        "still there after 5s");
                    if (!st->optional) {
                        if (out != NULL)
                            fprintf(out, "#   FAILED - stopping\n");
                        { run_cleanup(p, i + 1, out);
              vlhe_journal_end(); return i + 1; }
                    }
                }
            }
            continue;
        }

        /*
         * A REFUSAL STOPS THE PLAN AND RUNS NOTHING.
         *
         * Parenthesised for the same two reasons "(stop ...)" is: a
         * dry run reads as prose, and nothing can ever hand it to
         * execvp. It is checked BEFORE step_is_note() because a note
         * is skipped and a refusal must not be - that ordering is
         * the whole difference between the two.
         *
         * IT EXISTS FOR THE DEVICE-0 CONTEST. plan_load() emits one
         * when something we cannot identify holds audio device 0:
         * continuing would insmod vsound into a slot where it never
         * receives /dev/dsp, and the failure would surface three
         * steps later as vmidid reporting a busy MIDI slot. Better
         * to stop where the cause is.
         *
         * `optional' IS NOT CONSULTED. A refusal is the plan saying
         * it should not proceed, which is not a thing that can be
         * tolerated and stepped over; a step that may be skipped
         * should be a note instead.
         */
        /*
         * THE NODE STEP. Parenthesised like "(stop ...)" so a dry run
         * reads as prose and execvp can never see it; the runner does
         * the work in C because /dev/vdiscctl's minor is only known
         * once the module is in.
         *
         * THE NUMBERS ARE RE-READ FROM THE CONFIG rather than parsed
         * back out of the step text. The text is for the reader; the
         * config is the authority, and parsing our own prose would be
         * a second place for the two to disagree - which is the exact
         * class of bug this step exists to fix.
         */
        if (strncmp(st->cmd, "(nodes ", 7) == 0) {
            struct vlhe_modopts cd;

            if (vlhe_modopts(&cd) == 0) {
                /* A MAJOR OF 0 ASKED THE KERNEL FOR ONE - design/47 L4.
                 * The nodes then need the major the module GOT, which
                 * vlhe_modopts() reads from /proc/devices now that the
                 * insmod two steps back has run; make_nodes() used to
                 * return early on 0 and make nothing, and the unload
                 * sweep then found nothing to remove. Reachable only
                 * from a hand-edited file - the page's menus and the
                 * setter refuse 0 - but a file is a way in. */
                int maj = cd.major > 0 ? cd.major : cd.major_applied;

                if (out != NULL) {
                    shell_vdisc_nodes(out, maj, cd.ndevs);
                    fflush(out);
                }
                if (make_nodes(maj, cd.ndevs, out) != 0
                    && !st->optional) {
                    if (out != NULL)
                        fprintf(out, "#   FAILED - stopping\n");
                    { run_cleanup(p, i + 1, out);
              vlhe_journal_end(); return i + 1; }
                }
            } else if (out != NULL) {
                fprintf(out, "#   cannot read the CD settings -"
                             " no nodes made\n");
            }
            continue;
        }

        /*
         * `/dev/cdrom', both directions. Neither takes an argument -
         * everything they need is in the config and in the saved
         * file, so there is nothing to parse back out of the step.
         */
        if (strcmp(st->cmd, "(cdromlink)") == 0) {
            if (out != NULL) {
                shell_cdromlink(out);
                fflush(out);
            }
            if (link_cdrom_set(out) != 0 && !st->optional) {
                if (out != NULL)
                    fprintf(out, "#   FAILED - stopping\n");
                { run_cleanup(p, i + 1, out);
                  vlhe_journal_end(); return i + 1; }
            }
            continue;
        }

        /*
         * PUT THE REDIRECTED NODE BACK - AND THIS HANDLER WAS
         * MISSING ENTIRELY UNTIL 2026-09-25.
         *
         * `(dsprestore)' had one handler and it was inside
         * `run_cleanup()', which runs only after a FAILED load. So
         * on an ordinary unload the step sat in the plan and was
         * skipped as an unrecognised note - no output, no failure,
         * nothing. The journal then printed "the machine is as it
         * was before the apply" over a symlink that was still there.
         *
         * THE TARGET RUN THAT FOUND IT: nine changes applied, eight
         * undone, and the session file's own line still reading
         * `link /dev/dsp | node 14 3 | OPEN' while the three node
         * records beside it read UNDONE. The records were right and
         * nothing consumed this one.
         *
         * `(cdromrestore)' HAS ALWAYS HAD BOTH - one handler here
         * and one in the cleanup - which is why the cdrom half of
         * the same feature worked and this half did not.
         */
        /* SAVE MIXER LEVELS - every card's mixer, to its own file
         * (vlhe_mixer.c). Never fails the plan; says what happened. */
        if (strcmp(st->cmd, "(mixersave)") == 0) {
            int r = vlhe_mixer_save_all(NULL);

            if (out != NULL) {
                if (r > 0)
                    fprintf(out, "#   mixer levels: %d card(s) saved to"
                                 " %s\n", r, vlhe_mixer_state_path());
                else if (r == 0)
                    fprintf(out, "#   mixer levels: no mixer answered -"
                                 " nothing saved\n");
                else
                    fprintf(out, "#   mixer levels: could not write %s -"
                                 " NOT saved\n", vlhe_mixer_state_path());
                fflush(out);
            }
            continue;
        }
        if (strcmp(st->cmd, "(mixerrestore)") == 0) {
            int r = vlhe_mixer_restore_all(NULL);

            if (out != NULL) {
                if (r > 0)
                    fprintf(out, "#   mixer levels: %d card(s) restored"
                                 " from %s\n", r, vlhe_mixer_state_path());
                else if (r == 0)
                    fprintf(out, "#   mixer levels: none saved yet, or no"
                                 " card matched - left as they are\n");
                else
                    fprintf(out, "#   mixer levels: could not read %s -"
                                 " NOT restored\n", vlhe_mixer_state_path());
                fflush(out);
            }
            continue;
        }
        /* SAVE PROGRAM LEVELS - vsound's per-program table to its
         * own file and back (vlhe_backend.c). Never fails the plan. */
        if (strcmp(st->cmd, "(progsave)") == 0) {
            int r = vlhe_progvol_save_all(NULL);

            if (out != NULL) {
                if (r >= 0)
                    fprintf(out, "#   program levels: %d saved to %s\n",
                            r, vlhe_progvol_state_path());
                else
                    fprintf(out, "#   program levels: could not read"
                                 " vsound or write %s - NOT saved\n",
                            vlhe_progvol_state_path());
                fflush(out);
            }
            continue;
        }
        if (strcmp(st->cmd, "(progrestore)") == 0) {
            int r = vlhe_progvol_restore_all(NULL);

            if (out != NULL) {
                if (r > 0)
                    fprintf(out, "#   program levels: %d restored from"
                                 " %s\n", r, vlhe_progvol_state_path());
                else if (r == 0)
                    fprintf(out, "#   program levels: none saved yet -"
                                 " every program starts at 100%%\n");
                else
                    fprintf(out, "#   program levels: could not read %s"
                                 " or reach vsound - NOT restored\n",
                            vlhe_progvol_state_path());
                fflush(out);
            }
            continue;
        }
        if (strcmp(st->cmd, "(dsprestore)") == 0) {
            int n;
            struct vlhe_sess_rec *rec = vlhe_session_read_all(&n);
            int i;

            if (n < 0) {
                /* CANNOT READ THE UNDO LIST - say so. A silent skip
                 * is what this whole bug was. */
                if (out != NULL)
                    fprintf(out, "#   %s: cannot read - the undo list"
                                 " is unavailable, /dev/dsp not"
                                 " restored\n", vlhe_session_path());
                continue;
            }

            /* THE LINKS - restore_recorded_links(), shared with
             * run_cleanup(); it reads the file itself, and marking
             * does not reorder records, so `rec' below still lines up
             * for the node pass. */
            (void) restore_recorded_links(out);

            /*
             * AND THE CARD'S BORROWED NAME, WHICH IS A NODE RECORD -
             * design/36 row 80, 2026-09-26.
             *
             * IT BELONGS HERE RATHER THAN IN `(rmnodes ...)', AND
             * THAT IS THE WHOLE REASON THIS IS NOT A TWO-LINE FIX.
             * `remove_recorded_nodes()' splits its work by whether
             * the path contains "vmidi", so a `/dev/dspN' record
             * falls to the CD pass - and `(rmnodes cd)' is only
             * added `if (in_scope(VLHE_ENABLE_CD))'. A user with CD
             * unticked would leave the borrowed node behind and the
             * stock one missing, which is the bug this row is about,
             * one layer deeper.
             *
             * `(dsprestore)' IS UNCONDITIONAL - it is in every
             * unload plan - so the node is put right whatever is
             * enabled, which is what "the machine is as it was"
             * requires.
             */
            for (i = n - 1; i >= 0; i--) {
                int maj, min;

                if (rec[i].kind != VLHE_SESS_NODE
                    || !rec_pending(&rec[i])
                    || strncmp(rec[i].what, "/dev/dsp", 8) != 0)
                    continue;

                /* recorded_ok() - design/55 recommendation 2. */
                if (!recorded_ok(rec[i].what, rec[i].prior, out)) {
                    vlhe_journal_failed(VLHE_CH_RMNODE, rec[i].what,
                                        "not a path and state VLHE makes"
                                        " - left alone");
                    (void) vlhe_session_mark(i, VLHE_SESS_FAILED,
                                             "not a path VLHE makes"
                                             " - left alone");
                    continue;
                }

                if (out != NULL) {
                    shell_restore(out, rec[i].what, rec[i].prior);
                    fflush(out);
                }

                if (unlink(rec[i].what) != 0 && errno != ENOENT) {
                    if (out != NULL)
                        fprintf(out, "#   %s: cannot remove - %s\n",
                                rec[i].what, strerror(errno));
                    vlhe_journal_failed(VLHE_CH_RMNODE, rec[i].what,
                                        strerror(errno));
                    (void) vlhe_session_mark(i, VLHE_SESS_FAILED,
                                             strerror(errno));
                    continue;
                }
                if (out != NULL)
                    fprintf(out, "#   %s removed\n", rec[i].what);
                vlhe_journal_add(VLHE_CH_RMNODE, rec[i].what,
                                 rec[i].prior[0] != '\0'
                                     ? rec[i].prior : NULL);

                /* PUT MAKEDEV'S NODE BACK when there was one. An
                 * empty prior means the name was free before we
                 * borrowed it, so there is nothing to restore. */
                if (sscanf(rec[i].prior, "node %d %d", &maj, &min) == 2) {
                    if (remake_node(rec[i].what, 0, maj, min,
                                    rec[i].prior) == 0) {
                        if (out != NULL)
                            fprintf(out, "#   %s put back (c %d %d)\n",
                                    rec[i].what, maj, min);
                    } else {
                        /* NOT PUT BACK, SO NOT DONE - kept FAILED, so the
                         * next unload tries the mknod again (its unlink
                         * then finds nothing, which is fine). */
                        if (out != NULL)
                            fprintf(out, "#   %s: could not put the stock"
                                         " node back - %s\n",
                                    rec[i].what, strerror(errno));
                        vlhe_journal_failed(VLHE_CH_RMNODE, rec[i].what,
                                            strerror(errno));
                        (void) vlhe_session_mark(i, VLHE_SESS_FAILED,
                                                 "stock node not put back");
                        continue;
                    }
                }
                (void) vlhe_session_mark(i, VLHE_SESS_UNDONE, NULL);
            }
            free(rec);
            continue;
        }

        if (strcmp(st->cmd, "(cdromrestore)") == 0) {
            /*
             * SILENT WHEN THERE IS NOTHING TO DO. This step is in
             * EVERY unload plan and on the load plan's cleanup path,
             * so on a machine that never used the option it would
             * otherwise print a line per run saying nothing happened.
             */
            struct vlhe_cdrom_link st2;

            if (vlhe_cdrom_link(&st2) != 0) {
                if (out != NULL)
                    fprintf(out, "#   /dev/cdrom: cannot read its state -"
                                 " not restored\n");
                continue;
            }
            if (st2.saved[0] == '\0')
                continue;

            if (out != NULL) {
                shell_cdromrestore(out, st2.saved);
                fflush(out);
            }
            if (link_cdrom_restore(out) != 0 && !st->optional) {
                if (out != NULL)
                    fprintf(out, "#   FAILED - stopping\n");
                { run_cleanup(p, i + 1, out);
                  vlhe_journal_end(); return i + 1; }
            }
            continue;
        }

        /*
         * REMOVING THE NODES, on the way out.
         *
         * THE SESSION RECORDS FIRST - they name the nodes this load
         * actually made, so nothing is derived from a config that
         * may have changed since (design/40). The sweep below is the
         * FALLBACK, for a machine loaded by an older build that left
         * no records; there the major is re-read from the config and,
         * if it cannot be read, remove_nodes() removes NOTHING rather
         * than guessing which nodes were ours.
         */
        if (strncmp(st->cmd, "(rmnodes ", 9) == 0) {
            struct vlhe_modopts cd;
            int                 want_cd, want_midi;

            want_cd   = strcmp(st->cmd, "(rmnodes cd)") == 0;
            want_midi = strcmp(st->cmd, "(rmnodes midi)") == 0;

            if (out != NULL) {
                if (want_midi)
                    shell_rmnodes_midi(out);
                else
                    shell_rmnodes_cd(out, vlhe_modopts(&cd) == 0
                                          ? cd.ndevs : 1);
                fflush(out);
            }
            {
                int rc = remove_recorded_nodes(want_midi, out);

                if (rc > 0)
                    continue;   /* the records covered it */
                if (rc < 0) {
                    /* a record could not be removed - said above, and
                     * the step fails as any other would */
                    if (!st->optional) {
                        if (out != NULL)
                            fprintf(out, "#   FAILED - stopping\n");
                        { run_cleanup(p, i + 1, out);
                          vlhe_journal_end(); return i + 1; }
                    }
                    continue;
                }
            }

            if (vlhe_modopts(&cd) == 0) {
                if (remove_nodes(want_cd ? cd.major : -1,
                                 want_midi, out) != 0 && !st->optional) {
                    if (out != NULL)
                        fprintf(out, "#   FAILED - stopping\n");
                    { run_cleanup(p, i + 1, out);
                      vlhe_journal_end(); return i + 1; }
                }
            } else if (out != NULL) {
                fprintf(out, "#   cannot read the CD settings -"
                             " no nodes removed\n");
            }
            continue;
        }

        /*
         * THE vmidi NODE. Same shape as "(nodes ...)" above and for
         * the same reason - the minor is re-read from the config
         * rather than parsed back out of our own prose.
         */
        if (strcmp(st->cmd, "(dspnode)") == 0) {
            int idx = vlhe_vsound_dsp();
            const char *use = "/dev/dsp";
            struct vlhe_sound so;

            /* WHICH NAME TO REDIRECT - the user's setting, read here
             * rather than built into the step, because the step is a
             * pseudo-command with no room for it and the config may
             * have changed since the plan was printed. Read once, for
             * the printed line and the redirect below alike. */
            if (vlhe_sound(&so) == 0 && so.programs_use[0] != '\0')
                use = so.programs_use;

            if (out != NULL) {
                if (idx >= 0)
                    shell_dspnode(out, idx, use);
                fflush(out);
            }
            if (idx < 0) {
                /*
                 * NO /proc/vsound. Either the module did not load, or
                 * it predates the entry - a staged image can be older
                 * than the GUI reading it. Neither is worth stopping
                 * a load for: say so and carry on, because the common
                 * machine has /dev/dsp already and needs nothing.
                 */
                if (out != NULL)
                    fprintf(out, "#   /proc/vsound is not there -"
                                 " cannot tell which node vsound took."
                                 " An older module does not publish"
                                 " it\n");
            } else {
                /* SAY IT WHATEVER HAPPENS NEXT - design/38 9g: the
                 * node is the one fact the symlink hides, and a user
                 * wants it for aumix -d, kmix and /proc/sound. */
                if (out != NULL) {
                    char nodename[VLHE_PATH_MAX];

                    if (idx == 0)
                        strcpy(nodename, "/dev/dsp");
                    else
                        sprintf(nodename, "/dev/dsp%d", idx);
                    fprintf(out, "#   vsound is %s - point players"
                                 " here, and aumix -d / kmix at the"
                                 " card\n", nodename);
                }
                if (make_dsp_node(idx, out) != 0 && !st->optional) {
                    if (out != NULL)
                        fprintf(out, "#   FAILED - stopping\n");
                    { run_cleanup(p, i + 1, out);
                      vlhe_journal_end(); return i + 1; }
                }
            }

            /*
             * AND POINT THE USER'S DEVICE AT IT - design/38 section 9,
             * design/36 row 86.
             *
             * THIS SITS OUTSIDE THE `else' ABOVE, AND THAT PLACEMENT
             * IS THE WHOLE FIX. It used to be nested inside the
             * branch that CREATES vsound's node, so on any machine
             * where the node already existed `make_dsp_node()' had
             * nothing to do and the redirect was skipped with it -
             * silently, since the journal counts steps and not the
             * work inside one.
             *
             * MEASURED ON TARGET 2026-09-26: the first load of a boot
             * made `/dev/dsp3' and redirected; the unload removed the
             * node; the next two loads found `/dev/dsp3' already
             * there and made NO redirect at all, while the journal
             * reported `9 change(s)' either way. `ProgramsUse =
             * /dev/dsp2' was in the saved config and ignored, so
             * programs opening dsp2 reached the Sound Blaster
             * directly and never the mixer - the feature silently
             * absent on every load but the first.
             *
             * THE TWO STEPS ARE INDEPENDENT AND ONLY LOOKED RELATED:
             * one ensures vsound HAS a node, the other points a name
             * at it. The second needs the first to have SUCCEEDED,
             * not to have DONE anything - and an existing node
             * satisfies that just as well as a new one.
             *
             * `idx < 0' IS STILL THE ONE CASE THAT SKIPS IT, because
             * without /proc/vsound there is no node to point at.
             */
            if (idx >= 0) {
                if (link_dsp_set(use, idx, out) != 0
                    && !st->optional) {
                    if (out != NULL)
                        fprintf(out, "#   FAILED - stopping\n");
                    run_cleanup(p, i + 1, out);
                    vlhe_journal_end();
                    return i + 1;
                }
            }
            continue;
        }

        if (strncmp(st->cmd, "(vmidinode ", 11) == 0) {
            struct vlhe_midiopts mo;

            if (vlhe_midiopts(&mo) == 0) {
                if (out != NULL) {
                    shell_vmidinode(out, mo.minor);
                    fflush(out);
                }
                if (make_vmidi_node(mo.minor, out) != 0
                    && !st->optional) {
                    if (out != NULL)
                        fprintf(out, "#   FAILED - stopping\n");
                    { run_cleanup(p, i + 1, out);
                      vlhe_journal_end(); return i + 1; }
                }
            } else if (out != NULL) {
                fprintf(out, "#   cannot read the MIDI settings -"
                             " /dev/vmidi not checked\n");
            }
            /*
             * AND SAY WHICH MIDI DEVICE vmidi IS - design/36 row 62.
             * The user: "The load needs to tell what device vmidid is.
             * I tried several." With a card loaded there are two, in
             * load order, and lxdoom/musserv want the index.
             */
            {
                int sd = vlhe_midi_seqdev();

                if (out != NULL) {
                    if (sd >= 0)
                        fprintf(out, "#   vmidi is MIDI device %d"
                                     " - lxdoom/musserv: -u %d\n", sd, sd);
                    else
                        fprintf(out, "#   vmidi's MIDI device number"
                                     " could not be read from"
                                     " /proc/sound\n");
                }
            }
            continue;
        }

        if (strncmp(st->cmd, "(refuse ", 8) == 0) {
            if (out != NULL) {
                const char *why = st->cmd + 8;
                size_t      n   = strlen(why);

                fprintf(out, "# STOP: %.*s\n",
                        (int)(n > 0 && why[n - 1] == ')' ? n - 1 : n), why);
                if (st->why[0] != '\0')
                    fprintf(out, "#   %s\n", st->why);
                fflush(out);
            }
            { run_cleanup(p, i + 1, out);
              vlhe_journal_end(); return i + 1; }
        }

        if (step_is_note(st->cmd)) {
            if (out != NULL)
                fprintf(out, "#   %s\n", st->cmd);
            continue;
        }

        /* EXPANDED ONCE, PRINTED, THEN RUN - the printed bytes are the
         * executed bytes. Before it runs, SO A WATCHER SEES IT. */
        strncpy(cmdbuf, st->cmd, sizeof cmdbuf - 1);
        cmdbuf[sizeof cmdbuf - 1] = '\0';
        expand_nodes(cmdbuf, sizeof cmdbuf, cmd_resolves_own_out(st->cmd));
        if (out != NULL) {
            shell_expanded(out, cmdbuf, st->kind == VLHE_STEP_DAEMON);
            note_account(out, cmdbuf, st->kind == VLHE_STEP_DAEMON);
            fflush(out);
        }

        /*
         * ALREADY DONE IS DONE - design/54 D01, 2026-10-03. A load step
         * whose module is resident, or whose daemon is running, is
         * SATISFIED: it is skipped, not run. It used to run, and a
         * REQUIRED one then failed the press - the user's 2026-09-29
         * route, "i loaded without a sf2 picked, picked a sf2 and hit
         * load": vsound was up from the first press, `insmod vsound.o'
         * failed with "ALREADY LOADED. Unload first", the summary said
         * a component had failed and vsound's clean-up ran, though the
         * synth started in the same press. Not journalled - we changed
         * nothing - and `vlhe apply -u' still balances.
         */
        {
            char why[32];

            if (vlhe_apply_step_satisfied(st, why, sizeof why)) {
                if (out != NULL)
                    fprintf(out, "#   %s - nothing to do\n", why);
                continue;
            }
        }

        /* WAS IT ALREADY RESIDENT? Asked BEFORE the command, because
         * afterwards every successful load looks the same. Only a
         * module load can answer; everything else is 0. Since D01 the
         * check above skips a resident module first, so this is 1 only
         * when it arrived in between - kept for that race. */
        {
            char mod[64];

            step_module(st, mod, sizeof mod);
            pre_loaded = mod[0] != '\0'
                         && vlhe_status_module_loaded(mod);
        }

        rc = run_expanded(cmdbuf, st->kind == VLHE_STEP_DAEMON);

        /* A MODULE CAME OR WENT, OR TRIED TO - whatever the backend
         * remembered about vdisc's drive count is suspect now (B5). */
        if (st->kind == VLHE_STEP_MODULE)
            vlhe_drive_count_forget();

        if (rc == 0) {
            /*
             * RECORDED ONLY ON SUCCESS - vlhe_journal.h is emphatic:
             * a journal listing attempts lies about the machine's
             * state, and its whole value is that every line names
             * something real.
             *
             * THE KIND COMES FROM THE COMMAND'S FIRST WORD, not from
             * st->kind, because a MODULE step can be either
             * direction: the device-0 contest rmmods the card on the
             * way IN, and recording that as a load would make the
             * apply and undo fail to match. The verb is the fact.
             */
            /*
             * A LOAD THAT CHANGED NOTHING IS NOT A CHANGE.
             *
             * `modprobe soundcore' is in EVERY scope - vsound and
             * vmidi both import from it, and a CD-only trial may be
             * the first thing to need it - so the GUI's three scoped
             * runs wrote it to the journal THREE TIMES for one press.
             * The user's filed run of 2026-09-23 has exactly that.
             *
             * MODPROBE SUCCEEDS EITHER WAY: it returns 0 whether it
             * loaded the module or found it already there, so the
             * exit status cannot tell them apart and the journal
             * recorded a no-op as a change.
             *
             * SO ASK WHETHER IT WAS ALREADY THERE, BEFORE RUNNING.
             * Checked here rather than at plan time because a plan is
             * built before it runs - the first scope's would still
             * have emitted the step on a bare machine, and the dedup
             * would have worked only by accident of the later scopes
             * being built after the first had executed.
             *
             * `vlhe apply -u' MUST STILL BALANCE, which is why this
             * is narrow: only a MODULE load, only when it was
             * resident BEFORE the step, and never an rmmod. We did
             * not load it, so we must not claim to - and the undo
             * correctly leaves it alone.
             */
            if (!pre_loaded)
                journal_step(st, out);
            else if (out != NULL)
                fprintf(out, "#   already there - not recorded\n");
            continue;
        }

        if (st->optional) {
            /*
             * NOT A FAILURE. A module already loaded and a daemon
             * already running both land here, and both mean the
             * machine is closer to the wanted state rather than
             * further from it.
             *
             * AND IT NOW SAYS WHICH, WHEN IT CAN TELL. "(optional,
             * rc=1 - continuing)" is true and unreadable: pressing
             * Load twice produced three of them in a row, on a
             * machine where all three modules were loaded and fine.
             * The user, 2026-09-23, reporting it as a failure point -
             * which is exactly how it reads.
             */
            char mod[64];

            step_module(st, mod, sizeof mod);

            /*
             * AN rmmod THAT FAILS IS NOT THE SAME KIND OF OPTIONAL,
             * and treating it as one let a real failure pass as
             * success - 86Box 2026-09-23, where `rmmod vdisc' failed
             * because an orphaned daemon still held it, the step was
             * optional, and the journal then said "the machine is as
             * it was" over an lsmod showing `vdisc 24372 1'.
             *
             * THE DIRECTION IS WHAT DECIDES. An insmod that fails
             * because the module is already in leaves the machine
             * CLOSER to what was asked for; an rmmod that fails
             * leaves something BEHIND. The first is not worth
             * reporting as a problem, the second always is.
             *
             * SO IT IS JOURNALLED AS NOT DONE while the run carries
             * on. Stopping a teardown halfway is worse (design/09's
             * single-user finding), but a verdict that claims the
             * machine is clean when a module is still loaded is the
             * journal lying - the same class as the zombie bug fixed
             * earlier today.
             */
            if (strncmp(st->cmd, "rmmod ", 6) == 0) {
                const char *what = st->cmd + 6;

                if (rc == RUN_TIMEOUT) {
                    char why[96];

                    sprintf(why, "rmmod did not finish in %d s - stopped;"
                                 " the machine may need attention",
                            g_last_timeout_s);
                    if (out != NULL)
                        fprintf(out, "#   %s\n", why);
                    vlhe_journal_failed(VLHE_CH_RMMODULE, what, why);
                    continue;
                }
                if (out != NULL)
                    fprintf(out, "#   STILL LOADED - something is"
                                 " using it (rc=%d)\n", rc);
                vlhe_journal_failed(VLHE_CH_RMMODULE, what,
                                    "it would not unload");
                continue;
            }

            if (out != NULL) {
                if (mod[0] != '\0' && vlhe_status_module_loaded(mod))
                    fprintf(out, "#   already loaded - nothing to"
                                 " do\n");
                else
                    fprintf(out, "#   (optional, rc=%d -"
                                 " continuing)\n", rc);
            }
            continue;
        }

        /*
         * AND SAY WHAT IT MEANS WHEN WE CAN TELL. A REQUIRED module
         * step that fails on a module ALREADY LOADED is the
         * press-Load-twice case, and "FAILED rc=1 - stopping" gives a
         * user nothing to act on - they cannot see that the answer is
         * Unload first. Found by the user 2026-09-23.
         */
        {
            char mod[64];
            char why[96];

            step_module(st, mod, sizeof mod);

            if (rc == RUN_TIMEOUT) {
                sprintf(why, "did not finish in %d s - stopped; the"
                             " machine may need attention",
                        g_last_timeout_s);
                if (out != NULL)
                    fprintf(out, "#   FAILED - %s\n", why);
            } else if (mod[0] != '\0' && vlhe_status_module_loaded(mod)) {
                sprintf(why, "already loaded (rc=%d)", rc);
                if (out != NULL)
                    fprintf(out, "#   FAILED rc=%d - %s is ALREADY"
                                 " LOADED.  Unload first, then Load.\n",
                            rc, mod);
            } else {
                sprintf(why, "it would not load (rc=%d)", rc);
                if (out != NULL)
                    fprintf(out, "#   FAILED rc=%d - stopping\n",
                            rc);
            }

            /*
             * AND THE FAILURE GOES IN THE JOURNAL, which it did not.
             *
             * THE USER'S ASK, 2026-09-23: "The journal should be
             * appended to a complete log of everything we touched and
             * put back." It appends already - what it did not do was
             * record a step that FAILED, because vlhe_journal_add()
             * is success-only by design (a journal listing attempts
             * lies about the machine's state).
             *
             * SO THE 19:26 ENTRY IN THE FILED RUN READ:
             *
             *     unload module sb
             *     load   module sb  io=0x220 ...
             *     3 change(s). `vlhe apply -u' reverses them.
             *
             * We moved the user's card and put it back, with NO
             * RECORD OF WHY - `insmod vsound.o' failed and left no
             * trace, so the one thing that explained the other two
             * was the only thing missing.
             *
             * vlhe_journal_failed() IS THE RIGHT CALL AND ALREADY
             * EXISTS. It marks the line NOT DONE, which is exactly
             * what this is: something we tried, that did not happen,
             * and that the reader needs in order to make sense of the
             * lines around it.
             */
            if (mod[0] != '\0')
                vlhe_journal_failed(VLHE_CH_MODULE, mod, why);
            else if (st->kind == VLHE_STEP_DAEMON)
                vlhe_journal_failed(VLHE_CH_DAEMON, st->cmd, why);
        }
        /* THE PATH A FAILED `insmod vsound.o' TAKES, and the one this
         * whole mechanism was built for. */
        run_cleanup(p, i + 1, out);
        { vlhe_journal_end(); return i + 1; }
    }

    /* AN UNLOAD FILES ITS SESSION, OR SAYS WHAT IS LEFT - design/54
     * D25 and D23. Before the journal closes, so the verdict can use
     * it. */
    if (p->unload)
        vlhe_apply_session_close(out);

    vlhe_journal_end();
    return 0;
}

/*
 * CLOSE THE SESSION AT THE END OF AN UNLOAD - design/54 D25, 2026-10-03.
 *
 * design/40 4e (the user, 2026-09-25): a session whose records are all
 * UNDONE is finished, and is renamed with a timestamp into `sessions/'
 * so the next Load starts a fresh file; one with anything OPEN or FAILED
 * stays, because the machine still carries it. vlhe_session_rotate()
 * did that from the day it was written - AND NOTHING EVER CALLED IT. So
 * the live file never closed, every Load appended to it, and on 86Box
 * eleven Loads made 85 records, past the readers' 64 (D22), hiding the
 * one that held /dev/dsp's original node.
 *
 * CALLED AFTER EVERY UNLOAD PLAN, scoped or not: the rotation itself
 * refuses while any record is open, so a CD-only unload with Sound
 * still up keeps the file, and the last scope of a full unload files
 * it. Then the journal is told how many records are still in effect,
 * which is what its verdict now rests on (D23).
 *
 * "STILL IN EFFECT" IS SAID ONCE, FOR THE WHOLE PRESS - 86Box
 * 2026-10-03. The GUI runs one plan per component, and the CD scope's
 * close printed "N record(s) still in effect" while the MIDI and Sound
 * scopes had yet to run. Inside a nested journal run (one scope of
 * several) this files and counts but does not say what is left; the
 * caller running
 * the scopes calls it once more after the last, and that call reports.
 */
/*
 * THE UNLOAD'S SECOND LINE - design/54 7h Stage 3.4: how the machine
 * compares with the baseline once the undo is done. REPORTED, NOT
 * RESTORED - drift the user kept is theirs, and VLHE's own leftovers are
 * put back by the next Load (3.3). Paths a session record still holds
 * in effect are skipped: those are counted on the first line.
 */
static void
baseline_verdict(FILE *out)
{
    struct vlhe_base_ent *base;
    struct vlhe_sess_rec *rec;
    char now[VLHE_PATH_MAX], orig[VLHE_PATH_MAX], list[400];
    int  nb, ns, i, nd = 0;

    base = vlhe_baseline_read_all(&nb);
    if (nb <= 0) {
        free(base);
        return;                 /* no baseline: nothing to say */
    }
    rec = vlhe_session_read_all(&ns);
    if (ns < 0)
        ns = 0;
    list[0] = '\0';
    for (i = 0; i < nb; i++) {
        if (path_in_effect(rec, ns, base[i].what))
            continue;
        vlhe_baseline_describe(base[i].what, now, sizeof now);
        if (vlhe_state_same(now, base[i].state))
            continue;
        nd++;
        if (out != NULL)
            fprintf(out, "#   differs from the baseline: %s - was %s, now"
                         " %s%s\n", base[i].what,
                    base[i].how == VLHE_BASE_UNKNOWN ? "unknown"
                                                     : base[i].state,
                    now,
                    vlhe_apply_baseline_infer(base[i].what, now, orig,
                                              sizeof orig) != VLHE_BASE_FOUND
                        ? " (VLHE's leftover - the next Load puts it back)"
                        : "");
        if (strlen(list) + strlen(base[i].what) + 3 < sizeof list) {
            if (list[0] != '\0')
                strcat(list, ", ");
            strcat(list, base[i].what);
        }
    }
    if (out != NULL && nd == 0)
        fprintf(out, "#   compared with the baseline: as it was\n");
    vlhe_journal_baseline(nd, list);
    free(base);
    free(rec);
}

void
vlhe_apply_session_close(FILE *out)
{
    struct vlhe_sess_rec *rec;
    int r, n, i, still = 0;

    r = vlhe_session_rotate();
    if (r == 1) {
        if (out != NULL)
            fprintf(out, "#   session complete - filed in %s\n",
                    vlhe_session_dir());
        vlhe_journal_outstanding(0);
        if (!vlhe_journal_nested())
            baseline_verdict(out);
        return;
    }

    rec = vlhe_session_read_all(&n);
    if (n < 0) {
        if (out != NULL)
            fprintf(out, "#   %s: cannot read - whether anything is still"
                         " in effect is not known\n", vlhe_session_path());
        vlhe_journal_outstanding(-1);
        return;
    }
    /* CONFLICT IS FINAL (7h decision 5) and is not "still in effect":
     * only OPEN and FAILED keep the machine carrying something. */
    for (i = 0; i < n; i++)
        if (rec[i].state == VLHE_SESS_OPEN
            || rec[i].state == VLHE_SESS_FAILED)
            still++;
    free(rec);

    /* A LATER SCOPE MAY STILL UNDO THESE - said by the outermost call. */
    if (still > 0 && out != NULL && !vlhe_journal_nested())
        fprintf(out, "#   %d record(s) still in effect - kept in %s for"
                     " the next unload\n", still, vlhe_session_path());
    vlhe_journal_outstanding(still);
    if (!vlhe_journal_nested())
        baseline_verdict(out);
}

int
vlhe_plan_build(struct vlhe_plan *out, int unload)
{
    /* EVERYTHING ENABLED - the whole-stack plan, and the only caller
     * of the scoped one that existed before scoping did. */
    return vlhe_plan_build_scoped(out, unload, VLHE_SCOPE_ALL);
}

int
vlhe_plan_build_scoped(struct vlhe_plan *out, int unload, int which)
{
    return vlhe_plan_build_forced(out, unload, which, 0);
}

int
vlhe_plan_build_forced(struct vlhe_plan *out, int unload, int which,
                       int forced)
{
    if (out == NULL)
        return -1;

    /*
     * A SCOPE THAT NAMES NOTHING IS A PROGRAMMING ERROR, not an empty
     * plan - refused rather than silently building the whole stack,
     * which is what a bad constant would otherwise do.
     */
    if (which != VLHE_SCOPE_ALL
        && which != VLHE_ENABLE_SOUND
        && which != VLHE_ENABLE_MIDI
        && which != VLHE_ENABLE_CD)
        return -1;

    memset(out, 0, sizeof *out);
    out->unload = unload ? 1 : 0;

    g_scope = which;
    /* NEVER FOR THE WHOLE STACK, AND NEVER ON AN UNLOAD. Forcing
     * VLHE_SCOPE_ALL would mean "load everything regardless of the
     * checkboxes", which no caller wants and no button offers;
     * forcing a teardown is meaningless because in_scope() already
     * ignores `enabled' there. */
    g_scope_forced = (forced && !unload && which != VLHE_SCOPE_ALL) ? 1 : 0;
    if (unload)
        plan_unload(out);
    else
        plan_load(out);
    /* CLEARED ON EVERY PATH, so a later unscoped build cannot inherit
     * it. There is no early return between the set and here. */
    g_scope = VLHE_SCOPE_ALL;
    g_scope_forced = 0;

    return out->n;
}

/* ------------------------------------------------------------------ */
/* Printing                                                           */
/* ------------------------------------------------------------------ */

static const char *
kind_name(int kind)
{
    switch (kind) {
    case VLHE_STEP_MODULE:  return "module";
    case VLHE_STEP_DAEMON:  return "daemon";
    case VLHE_STEP_NODE:    return "node";
    case VLHE_STEP_CHECK:   return "check";
    default:                return "?";
    }
}

void
vlhe_plan_print(const struct vlhe_plan *p, FILE *fp)
{
    int i;

    if (p == NULL || fp == NULL)
        return;

    for (i = 0; i < p->n; i++) {
        const struct vlhe_step *s = &p->step[i];

        fprintf(fp, "  %-7s %s\n", kind_name(s->kind), s->cmd);
        if (s->why[0] != '\0')
            fprintf(fp, "          %s%s\n", s->why,
                    s->optional ? " (optional)" : "");
    }

    if (p->truncated)
        fprintf(fp, "  WARNING: the plan was truncated at %d steps\n",
                VLHE_PLAN_MAX);
}

/*
 * THE SIMULATION - the user's design, 2026-10-01: "A button labeled
 * simulate. This would show the text 'Commands run on load' and show
 * every command run a full load would show (can use the check boxes
 * to determine what is loaded), then 'Commands on unload', with the
 * option for a user to save this output." Nothing is run and nothing
 * is probed; the three run-time names are STATED ASSUMPTIONS ("assume
 * /dev/dsp is the real card and that vsound gets /dev/dsp1 and state
 * these assumptions"), and the fonts come from the settings, which
 * know them - or that there are none - without touching the machine.
 * It needs no root, which is the point: a first-run user can read
 * exactly what Load would do to their machine before deciding to let
 * it.
 */
#define SIM_CARD   "/dev/dsp"
#define SIM_VSOUND "/dev/dsp1"
#define SIM_VSOUND_IDX 1             /* vsound at dsp1 - minor 19 */

/* Append `with' to `out', bounded; a value with a space is quoted so
 * the line is a valid shell command. */
static char *
sim_put(char *out, char *end, const char *with, int quote)
{
    size_t n = strlen(with);
    int    q = quote && strchr(with, ' ') != NULL;

    if (q && out < end)
        *out++ = '"';
    if (out + n >= end)
        n = (size_t)(end - out);
    memcpy(out, with, n);
    out += n;
    if (q && out < end)
        *out++ = '"';
    return out;
}

/* The step's command with the run-time tags replaced by the stated
 * assumptions.
 *
 * NO DIRECTORY IS PREFIXED - the step already carries it. daemon_cmd()
 * builds `<dir>/vsoundd' when the daemon directory is known, so the
 * first version of this, which put daemon_dir() in front as well,
 * printed `/mnt/xfer//mnt/xfer/vsoundd' on the target
 * (tests/logs/2026-10-01-86box-buttons-prefs-crash-round/Simulate.log)
 * - and nothing on the workstation, where the directory is empty both
 * times, so the host test could not see it. */
static void
sim_expand(const struct vlhe_step *s,
           const struct vlhe_font *fonts, int nfonts,
           char *line, size_t max)
{
    const char *in = s->cmd;
    char *out = line;
    char *end = line + max - 1;

    while (*in != '\0' && out < end) {
        const char *with = NULL;
        size_t taglen = 0;

        if (strncmp(in, "@VSOUND:@CARD@@", 15) == 0) {
            with = SIM_CARD;  taglen = 15;      /* sound not in the load */
        } else if (strncmp(in, "@VSOUND@", 8) == 0) {
            with = SIM_VSOUND; taglen = 8;
        } else if (strncmp(in, "@CARD@", 6) == 0) {
            with = SIM_CARD;  taglen = 6;
        } else if (strncmp(in, "@DRIVES@", 8) == 0) {
            with = vlhe_conf_drives_path();  taglen = 8;
        } else if (strncmp(in, "@FONT", 5) == 0
                   && in[5] >= '0' && in[5] <= '9' && in[6] == '@') {
            int k = in[5] - '0';

            with = (k < nfonts && fonts[k].path[0] != '\0')
                   ? fonts[k].path : "(no font set in Midi Settings)";
            taglen = 7;
        }
        if (with != NULL) {
            out = sim_put(out, end, with, 1);
            in += taglen;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

/*
 * ONE STEP AS SHELL - the user's design, 2026-10-01: "the simulate
 * should show the commands that a user can type out to load and
 * unload everything by hand ... they should be able to copy the load
 * commands and create a load script and unload script". So every step
 * the plan runs INSIDE the program - a node made, a daemon signalled,
 * a link put back - is rendered as the shell that does the same thing,
 * and a step with no shell equivalent (a failure-path cleanup, a note)
 * is a comment or nothing. `verbose' adds the plan's own reason above
 * each line, as a comment, so the text is a valid script either way.
 */
static void
sim_shell_step(const struct vlhe_step *s, int unload, int verbose, FILE *fp,
               const struct vlhe_font *fonts, int nfonts, int *ndrives)
{
    const char *c = s->cmd;
    char line[VLHE_CMD_MAX + 256];

    if (verbose && s->why[0] != '\0'
        && strcmp(c, "(cdromrestore)") != 0 && strcmp(c, "(dsprestore)") != 0)
        fprintf(fp, "# %s%s\n", s->why, s->optional ? " (optional)" : "");

    if (strcmp(c, "(dspnode)") == 0) {
        shell_dspnode(fp, SIM_VSOUND_IDX, SIM_CARD);
        if (verbose)
            fprintf(fp, "#   /dev/dsp0 keeps the card reachable under its"
                        " own name; /dev/dsp now reaches the mixer\n");
        return;
    }
    if (strncmp(c, "(vmidinode minor ", 17) == 0) {
        shell_vmidinode(fp, atoi(c + 17));
        return;
    }
    if (strncmp(c, "(nodes vdisc major ", 19) == 0) {
        int major = atoi(c + 19), n = 1;
        const char *comma = strchr(c, ',');

        if (comma != NULL)
            n = atoi(comma + 1);
        if (n < 1)
            n = 1;
        if (ndrives != NULL)
            *ndrives = n;
        shell_vdisc_nodes(fp, major, n);
        if (verbose)
            fprintf(fp, "#   the control node's minor is handed out by the"
                        " kernel at insmod - /proc/misc says which\n");
        return;
    }
    if (strcmp(c, "(cdromlink)") == 0) {
        if (verbose)
            fprintf(fp, "#   note what /dev/cdrom points at first - the"
                        " unload puts it back\n");
        shell_cdromlink(fp);
        return;
    }
    if (strcmp(c, "(cdromrestore)") == 0) {
        if (!unload)
            return;             /* the load's failure-path cleanup */
        shell_cdromrestore(fp, NULL);
        return;
    }
    if (strcmp(c, "(dsprestore)") == 0) {
        if (!unload)
            return;
        /* the two the load makes, put back to stock - the run reads
         * the real ones from the session records */
        shell_restore(fp, SIM_CARD, "node 14 3");
        shell_restore(fp, "/dev/dsp0", NULL);
        return;
    }
    if (strcmp(c, "(mixersave)") == 0) {
        fprintf(fp, "# save every sound card's mixer levels to %s\n",
                vlhe_mixer_state_path());
        return;
    }
    if (strcmp(c, "(mixerrestore)") == 0) {
        fprintf(fp, "# restore every sound card's mixer levels from %s\n",
                vlhe_mixer_state_path());
        return;
    }
    if (strcmp(c, "(progsave)") == 0) {
        fprintf(fp, "# save each program's volume to %s\n",
                vlhe_progvol_state_path());
        return;
    }
    if (strcmp(c, "(progrestore)") == 0) {
        fprintf(fp, "# push each program's saved volume from %s into vsound\n",
                vlhe_progvol_state_path());
        return;
    }
    if (strcmp(c, "(rmnodes cd)") == 0) {
        shell_rmnodes_cd(fp, (ndrives != NULL && *ndrives > 0) ? *ndrives : 1);
        return;
    }
    if (strcmp(c, "(rmnodes midi)") == 0) {
        shell_rmnodes_midi(fp);
        return;
    }
    if (strncmp(c, "(stop ", 6) == 0) {
        char name[32];
        int  n = 0;

        c += 6;
        while (*c != '\0' && *c != ')' && n < (int) sizeof name - 1)
            name[n++] = *c++;
        name[n] = '\0';
        shell_stop(fp, name);
        return;
    }
    if (c[0] == '(') {
        /* any other note - "(sound.o is not removed)", "(vmidid - no
         * font configured)": nothing to run, say so when verbose */
        if (verbose)
            fprintf(fp, "#   %s\n", c);
        return;
    }

    /* A REAL COMMAND, as the plan wrote it - the daemon's directory is
     * already in it. Daemons start in the background. */
    sim_expand(s, fonts, nfonts, line, sizeof line);
    fprintf(fp, "%s%s\n", line, s->kind == VLHE_STEP_DAEMON ? " &" : "");
}

void
vlhe_apply_sim_step(const struct vlhe_step *s, FILE *fp)
{
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    int nfonts = vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL);
    int ndrives = 1;

    if (nfonts < 0)
        nfonts = 0;
    sim_shell_step(s, 0, 0, fp, fonts, nfonts, &ndrives);
}

int
vlhe_plan_simulate(FILE *fp, int verbose)
{
    struct vlhe_plan  p;
    struct vlhe_font  fonts[VLHE_MAX_FONTS];
    int nfonts, i, k, n = 0, ndrives = 1;

    if (fp == NULL)
        return -1;
    nfonts = vlhe_fonts_effective(fonts, VLHE_MAX_FONTS, NULL);
    if (nfonts < 0)
        nfonts = 0;

    /* NOT A SCRIPT - the user, 2026-10-01: "I didnt want it to actually
     * create a script I wanted the output to allow the user to create
     * a script." So no shebang, no set -e, and Save... offers a .txt;
     * the lines are still the commands, and still run as root. */
    fprintf(fp,
        "# VLHE - what Load would run for the components ticked on the\n"
        "# Status page, then what Unload would undo. NOTHING WAS RUN to\n"
        "# make this, and it assumes every command succeeds - a real load\n"
        "# stops at the first that does not. The commands need root.\n"
        "#\n"
        "# Assumed, because they are decided at run time:\n"
        "#   %s   is your real sound card\n"
        "#   %s  is where vsound lands once the card has moved\n"
        "#              aside (the next free node; /proc/vsound says\n"
        "#              which after a real load)\n",
        SIM_CARD, SIM_VSOUND);
    if (nfonts > 0 && fonts[0].path[0] != '\0')
        fprintf(fp, "#   the SoundFonts are the ones set in Midi Settings\n");
    else
        fprintf(fp, "#   no SoundFont is set in Midi Settings - the synth\n"
                    "#              would be started without one\n");

    for (k = 0; k < 2; k++) {
        if (k == 1)
            g_assume_loaded = 1;
        n = vlhe_plan_build(&p, k);
        g_assume_loaded = 0;
        /* THE TWO HEADINGS, the user's words, 2026-10-01. */
        fprintf(fp, "\n%s\n", k == 0
                ? "#---- loading with current settings would run the"
                  " following commands ----"
                : "#---- unload assumes everything is loaded and restores"
                  " the system to how it was prior ----");
        if (n <= 0) {
            fprintf(fp, "# (nothing - no component is ticked)\n");
            continue;
        }
        /*
         * A LINE PER COMPONENT GROUP - the user, 2026-10-01: "a comment
         * line in the simulation output matching the load and unload
         * comment format, something like # --- vsound --- ... so a user
         * can tell which commands are for what module". The builders
         * stamp each step with its component (g_comp); a heading goes
         * out whenever that changes, and the unload interleaves the
         * three by step kind - daemons, then modules, then nodes - so
         * its headings repeat. That is the order the commands must run
         * in, so the headings follow it rather than regroup it.
         */
        {
            int last = -1;

            for (i = 0; i < p.n; i++) {
                const struct vlhe_step *st = &p.step[i];
                int comp = st->comp;

                /* a step that prints nothing - a failure-path cleanup
                 * on the load side, a note when not verbose - gets no
                 * heading of its own */
                if (!k && (strcmp(st->cmd, "(cdromrestore)") == 0
                           || strcmp(st->cmd, "(dsprestore)") == 0))
                    continue;
                if (!verbose && st->cmd[0] == '('
                    && strncmp(st->cmd, "(dspnode)", 9) != 0
                    && strncmp(st->cmd, "(vmidinode", 10) != 0
                    && strncmp(st->cmd, "(nodes ", 7) != 0
                    && strncmp(st->cmd, "(cdromlink)", 11) != 0
                    && strncmp(st->cmd, "(cdromrestore)", 14) != 0
                    && strncmp(st->cmd, "(dsprestore)", 12) != 0
                    && strncmp(st->cmd, "(rmnodes ", 9) != 0
                    && strncmp(st->cmd, "(stop ", 6) != 0)
                    continue;
                shell_heading(fp, comp, &last);
                sim_shell_step(st, k, verbose, fp, fonts, nfonts, &ndrives);
            }
        }
        if (p.truncated)
            fprintf(fp, "# WARNING: the plan was truncated at %d steps\n",
                    VLHE_PLAN_MAX);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* THE KERNEL-LOG CAPTURE - [Tracing] Capture                         */
/* ------------------------------------------------------------------ */

/*
 * BROUGHT BACK 2026-10-02 as its own setting - the user: "Bring it back
 * for this. We need it for this round of testing. for release it is off
 * by default and do it as a separate flag".
 *
 * WHAT IT DOES - the old Status-page "Capture to file", and `load.sh'
 * before it: on Load, stop sysklogd (syslogd and klogd are one package,
 * and `cat /proc/kmsg' is a CONSUMING read, so klogd must be out of the
 * way), make run-<stamp>/ beside DAEMON.LOG, and read /proc/kmsg into
 * run-<stamp>/trace.log. On Unload, stop the reader, copy DAEMON.LOG in
 * and restart sysklogd - ONLY if we stopped it. Without it a traced
 * load goes to klogd and lands in /var/log/kern/kern.debug, mixed with
 * the machine's own messages (tests/logs/2026-10-02-86box-guest-var-log).
 *
 * ONLY WITH [Tracing] Enabled AS WELL, and only as root: stopping
 * sysklogd and reading /proc/kmsg both need it. Unraised, the old one
 * produced an EMPTY trace.log and said nothing (86Box, 2026-09-30) -
 * this one says why it skipped. The GUI raises around the calls.
 *
 * THE STATE IS IN FILES, NOT IN THIS PROCESS - trace.pid and trace.dir
 * beside DAEMON.LOG, as load.sh kept them. The old GUI version held the
 * reader's pid in a static, so only the same GUI session could close
 * it; a `vlhe apply -u' or a restarted control centre left `cat'
 * running and syslog stopped. Now any later unload closes it.
 *
 * AND THE SAVED PID IS CHECKED BEFORE IT IS SIGNALLED: > 0, and
 * /proc/<pid>/cmdline must be `cat /proc/kmsg'. A pid read from a file
 * can belong to anything after a reboot, and CLAUDE.md section 1 has
 * what an unchecked kill() cost this project.
 */

extern char **environ;        /* diag_run()'s child replaces it */

static int
diag_run(const char *file, const char *a1, const char *a2)
{
    pid_t kid = fork();
    int   st  = -1;

    if (kid < 0)
        return -1;
    if (kid == 0) {
        int null = open("/dev/null", O_WRONLY);

        if (null >= 0) {
            dup2(null, 1);
            dup2(null, 2);
            if (null > 2)
                close(null);
        }
        /*
         * ALL THREE IDS, NOT JUST THE EFFECTIVE ONE - kept from the
         * old capture. The callers are init scripts and /bin/sh is
         * bash: started with euid 0 and a real uid that is not, it
         * drops euid to the real uid (no -p), so a raise by seteuid(0)
         * alone - the setuid build's - would run `sysklogd stop' as
         * the user. setuid(0) is allowed because euid is 0. Nothing
         * happens unraised, the plain-root and CLI case.
         */
        /*
         * AND, WHEN THAT MAKES A USER ROOT, NONE OF THEIR ENVIRONMENT -
         * design/55 R12, design/54 D54 (2026-10-04). In the setuid
         * build the real uid is the user's, so setuid(0) here turns
         * their process into full root - and it kept their LANG, LC_*
         * and TZ, which an init script and dmesg then read as root
         * (glibc's secure mode no longer applies once all three ids are
         * 0). Such a child gets root's PATH and HOME and nothing else;
         * plain root (su -, the init script) keeps its own environment
         * as before.
         */
        if (geteuid() == 0) {
            static char *clean[] = {
                "PATH=/sbin:/usr/sbin:/bin:/usr/bin", "HOME=/", NULL
            };
            int was_user = (getuid() != 0);

            (void) setuid(0);
            if (was_user)
                environ = clean;
        }
        execlp(file, file, a1, a2, (char *) 0);
        _exit(127);
    }
    /* BOUNDED - design/54 D61: a syslog script that never returns must
     * not freeze the press either. */
    if (wait_bounded(kid, &st, RUN_MODULE_S) != 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The directory DAEMON.LOG is in, with its slash, or "" - the run
 * folder and the two state files go beside it. */
static void
capture_base(char *out, size_t max)
{
    const char *dl = vlhe_daemon_log_path();
    const char *slash = strrchr(dl, '/');

    out[0] = '\0';
    if (slash == NULL || (size_t) (slash - dl) + 2 > max)
        return;
    memcpy(out, dl, (size_t) (slash - dl) + 1);
    out[(slash - dl) + 1] = '\0';
}

/* Is `pid' a live `cat /proc/kmsg'? The only process this ever signals. */
static int
capture_pid_is_ours(long pid)
{
    char path[64], buf[64];
    int  fd, n;

    if (pid <= 0)
        return 0;
    sprintf(path, "/proc/%ld/cmdline", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = (int) read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    /* cmdline is NUL-separated: "cat\0/proc/kmsg\0" */
    return strcmp(buf, "cat") == 0
           && n > 4 && strcmp(buf + 4, "/proc/kmsg") == 0;
}

static void
capture_copy(const char *from, const char *to)
{
    char buf[4096];
    int  in, out, n;

    in = open(from, O_RDONLY);
    if (in < 0)
        return;
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return;
    }
    while ((n = (int) read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t) n) != n)
            break;              /* a full disc - stop, do not spin */
    close(in);
    close(out);
}

int
vlhe_capture_begin(FILE *out)
{
    char base[256], dir[300], file[320], log[320];
    time_t now;
    struct tm *tm;
    pid_t kid;
    int   stopped = 0;
    FILE *fp;
    long  old = 0;

    if (!vlhe_tracing() || !vlhe_trace_capture())
        return 0;
    if (geteuid() != 0) {
        if (out != NULL)
            fprintf(out, "#   kernel-log capture skipped: it needs root"
                         " (it stops syslog and reads /proc/kmsg)\n");
        return 0;
    }
    capture_base(base, sizeof base);
    if (base[0] == '\0')
        return 0;

    /* ONE AT A TIME - load.sh refused a second, and so does this. */
    sprintf(file, "%.250strace.pid", base);
    fp = fopen(file, "r");
    if (fp != NULL) {
        if (fscanf(fp, "%ld", &old) != 1)
            old = 0;
        fclose(fp);
        if (capture_pid_is_ours(old)) {
            if (out != NULL)
                fprintf(out, "#   kernel-log capture already running"
                             " (pid %ld) - left as it is\n", old);
            return 0;
        }
    }

    now = time(NULL);
    tm  = localtime(&now);
    if (tm == NULL)
        return 0;
    sprintf(dir, "%.250srun-%04d%02d%02d-%02d%02d%02d", base,
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        if (out != NULL)
            fprintf(out, "#   kernel-log capture: cannot make %s\n", dir);
        return 0;
    }

    /* STOP SYSLOG FIRST, AND REMEMBER THAT WE DID - the record is what
     * licenses the restart, so a machine whose syslog was already down
     * is not started by our unload. */
    if (diag_run("/etc/init.d/sysklogd", "stop", NULL) == 0)
        stopped = 1;

    /* CONSOLE LEVEL 4: the capture is the same either way (2.2 fills
     * log_buf and filters the console after), but a text console is
     * not flooded. load.sh did the same. */
    diag_run("dmesg", "-n", "4");

    sprintf(log, "%.300s/trace.log", dir);
    kid = fork();
    if (kid < 0) {
        if (stopped)
            diag_run("/etc/init.d/sysklogd", "start", NULL);
        return 0;
    }
    if (kid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0)
            _exit(127);
        dup2(fd, 1);
        if (fd > 1)
            close(fd);
        execlp("cat", "cat", "/proc/kmsg", (char *) 0);
        _exit(127);
    }

    /* THE STATE, for whichever process unloads. */
    fp = fopen(file, "w");
    if (fp != NULL) {
        fprintf(fp, "%ld\n", (long) kid);
        fclose(fp);
    }
    sprintf(file, "%.250strace.dir", base);
    fp = fopen(file, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n%s\n", dir, stopped ? "syslog-stopped" : "syslog-left");
        fclose(fp);
    }
    if (out != NULL)
        fprintf(out, "#   capturing the kernel log to %s/trace.log -"
                     " syslog is %s for this run\n", dir,
                stopped ? "STOPPED" : "not running anyway");
    return 1;
}

int
vlhe_capture_end(FILE *out)
{
    char  base[256], pidf[320], dirf[320], dir[300], how[32], dest[320];
    FILE *fp;
    long  pid = 0;
    int   early = 0, stopped = 0;

    capture_base(base, sizeof base);
    if (base[0] == '\0')
        return 0;
    sprintf(pidf, "%.250strace.pid", base);
    sprintf(dirf, "%.250strace.dir", base);

    /* GUARDED ON THE FILES EXISTING, NOT ON THE SETTING - as unload.sh
     * guarded on trace.pid/trace.dir: turning Capture off after a load
     * still closes the capture and brings syslog back. */
    fp = fopen(dirf, "r");
    if (fp == NULL)
        return 0;
    dir[0] = how[0] = '\0';
    if (fgets(dir, sizeof dir, fp) == NULL)
        dir[0] = '\0';
    if (fgets(how, sizeof how, fp) == NULL)
        how[0] = '\0';
    fclose(fp);
    dir[strcspn(dir, "\n")] = '\0';
    stopped = strncmp(how, "syslog-stopped", 14) == 0;

    fp = fopen(pidf, "r");
    if (fp != NULL) {
        if (fscanf(fp, "%ld", &pid) != 1)
            pid = 0;
        fclose(fp);
    }

    /* SIGNALLED ONLY IF IT IS STILL OUR READER - see the header. Gone
     * already means `cat' exited on its own (a full disc, usually) and
     * the capture ended before this unload; that is reported. */
    if (capture_pid_is_ours(pid)) {
        int st;

        kill((pid_t) pid, SIGTERM);
        /* BOUNDED - design/54 D61; reaps it if ours, returns at once
         * if it is not our child. */
        (void) wait_bounded((pid_t) pid, &st, RUN_OTHER_S);
    } else if (pid > 0) {
        early = 1;
    }

    sync();

    /* DAEMON.LOG JOINS THE TRACE - COPIED, not moved: the daemons hold
     * it open with `>>', and a moved file would leave them writing to
     * an unlinked inode. After the sync, so it is whole. */
    if (dir[0] != '\0') {
        sprintf(dest, "%.300s/DAEMON.LOG", dir);
        if (strcmp(vlhe_daemon_log_path(), dest) != 0)
            capture_copy(vlhe_daemon_log_path(), dest);
    }

    if (stopped)
        diag_run("/etc/init.d/sysklogd", "start", NULL);

    unlink(pidf);
    unlink(dirf);

    if (out != NULL) {
        if (early)
            fprintf(out, "#   the kernel-log capture had ended on its own"
                         " before the unload - disk full? trace.log stops"
                         " short; `dmesg' has the rest while it lasts\n");
        fprintf(out, "#   kernel-log capture closed: %s%s\n",
                dir[0] ? dir : "(no folder recorded)",
                stopped ? "; syslog restarted" : "");
    }
    return early ? 2 : 1;
}

