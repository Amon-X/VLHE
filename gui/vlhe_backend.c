/*
 * vlhe_backend.c - the REAL backend. What the control centre sees on a
 * machine, rather than the scripted state of vlhe_backend_fake.c.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHAT THIS FILE IS AND IS NOT. It is the join: the five pieces below
 * do the work, and this translates between them and vlhe_backend.h's
 * 43 functions.
 *
 *   vlhe_conf.c       the three config files, and the atomic write
 *   vlhe_conf_template.c  their keys, defaults and comments as data
 *   vlhe_mixer.c      /dev/mixer - the CARD's own levels
 *   vlhe_status.c     /proc/modules and the pid files
 *   vlhe_fontscan.c   the .sf2 search
 *
 * Each of those was host-tested before this existed and probed on
 * three machines (86Box, the Acer, the fresh install) on 2026-09-18.
 * THIS file is the part that had no test until the GUI ran against it.
 *
 * EVERY CALL FAILS SOFTLY. vlhe_backend.h is emphatic: "a machine with
 * no vsound loaded is a machine whose control centre still opens -
 * showing nothing rather than refusing to start". So a getter returns
 * a count that may be zero and never an error the caller must handle
 * before it can draw a window.
 *
 * THE DRIVES ARE READ-ONLY IN THIS VERSION. vlhe_attach() and
 * vlhe_detach() need a control channel to vdiscd that does not exist
 * yet (design/33 section 3f): /dev/vdiscctl admits ONE opener and that
 * is the daemon, and attaching needs the image parsed, which is the
 * daemon's 1500 lines. What IS read is what a running daemon has -
 * the drive count and, where the module tells us, its state.
 *
 * C89, GCC 2.95.2. No GTK types.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h> /* major()/minor(), the look-only probe */
#include <sys/time.h>     /* gettimeofday - the channel list kept 100 ms */
#include <sys/ioctl.h>
#include <linux/cdrom.h>  /* the CD-ROM layer's ioctls and structures */
#include <signal.h>
#include <time.h>      /* localtime, strftime - the backup date */
#include <sys/wait.h>  /* waitpid - Reset All Users writes in a child */
#include <grp.h>       /* setgroups - and that child drops root's groups */

#include "vlhe_backend.h"
#include "vlhe_rates.h"
#include "vdisc.h"       /* vdisc's own ioctls and structures */
#include "vdiscd_ctl.h"
#include "vmidid_ctl.h"      /* include/ - the one cross-tree include, gone with the tree */
#include "vlhe_conf.h"
#include "vlhe_self.h"
#include "vlhe_session.h"
#include "vlhe_conf_template.h"
#include "vlhe_fontscan.h"
#include "vlhe_mixer.h"
#include "vlhe_status.h"
#include "vlhe_modconf.h"  /* look-only opens, CLAUDE.md section 5 */
#include "vlhe_progvol.h"
#include "autovoice.h"     /* the AutoVoice* ranges - one copy, the daemon's */

#include "vsound.h"

/* ------------------------------------------------------------------ */
/* State this file keeps                                              */
/* ------------------------------------------------------------------ */

/*
 * THE CONFIG IS READ ONCE AND HELD. Re-reading on every getter would
 * mean a file open per poll at 250 ms, and the GUI is the only thing
 * that writes these files - so what we hold IS what is on disk.
 *
 * `sys' is /etc/vlhe.conf, `usr' the per-user one. The drive state
 * file is read on demand: a daemon writes it, so ours can be stale.
 */
/* THE LAST POLL'S CONTRIBUTED-BYTE COUNT, per slot, for deriving
 * `active' - see vlhe_channels(). Keyed on pid too, so a reused slot
 * does not inherit the previous client's history. */
static unsigned int last_mixed[VLHE_MAX_CHAN];
static int          last_pid[VLHE_MAX_CHAN];

/* The control channels, raised for the setuid build - defined with
 * vlhe_root_begin() further down, used from the drive code above it. */
static int vdisc_request(const char *req, char *reply, size_t max, int ms);
static int root_available(void);
static void system_file_claim(const char *path);
static void readback_note(const char *path);
static int  readback_apply(const char *path, char *note, int max);
static void readback_reapply(void);
static int  file_exists(const char *path);
static char   g_readback_path[VLHE_PATH_MAX];  /* the draft in use, or "" */
static time_t g_readback_when;
static int  seed_read_for(struct vlhe_conf *seed, int for_system,
                          char *why, int max);
static void apply_seed(struct vlhe_conf *c, const struct vlhe_conf *seed,
                       int which, int overwrite);
static int g_draft_pending;     /* an accepted draft, unsaved - R7 */
static int g_conf_trust_all;    /* the host tests' seam - see its setter */
static const char *const enable_section[3];    /* defined with Status */
static int vmidi_request(const char *req, char *reply, size_t max);

/*
 * OPEN A /dev/vdiscN NODE, RAISED - the review of design/48's fixes,
 * 2026-09-30.
 *
 * b48dcf7 stopped holding root for the session and wrapped the
 * control channels, and missed these: five functions open the drive
 * node DIRECTLY - transport, position, the lock (read and set) and
 * the drive count. make_nodes() creates the node 0660, owned by the
 * creating process's effective uid and gid. The setuid build's Load
 * makes it root plus the USER's group, since seteuid() changes the
 * uid only, and the user can open it; the init script makes it
 * root:root, and after Modify the CD pages would fail to reach it.
 *
 * ONLY THE open() IS RAISED. The permission check happens there, and
 * an open descriptor keeps its access. None of the ioctls these
 * functions issue checks a capability - vdisc_mod.c has no capable()
 * at all - so nothing runs as root once the node is open.
 *
 * TWO OF THE FIVE RUN IN THE CD+G POLL at 15 Hz. That is about
 * thirty seteuid() calls a second, which costs nothing, and the
 * raised work is one open() on a fixed path with no input from
 * anyone - not the "root in a poll" design/48 warns about, which is
 * root while processing something untrusted.
 */
static int
vdisc_node_open(const char *node)
{
    int fd;

    vlhe_root_begin();
    fd = open(node, O_RDONLY | O_NONBLOCK);
    vlhe_root_end();            /* keeps open's errno */
    return fd;
}

static struct vlhe_conf cfg_sys;
static struct vlhe_conf cfg_usr;
static int cfg_loaded;
/* WHY THE SYSTEM FILE MUST NOT BE ACTED ON, or empty if it may -
 * design/49 T0. Set whenever it is read; see vlhe_conf_system_trusted(). */
static char cfg_sys_untrusted[VLHE_PATH_MAX + 64];

/* THE PATHS THIS SESSION IS USING, fixed at load time - see
 * load_config() for why resolving them repeatedly was a bug. */
static char cfg_sys_path[VLHE_PATH_MAX];
static char cfg_usr_path[VLHE_PATH_MAX];
/* What VLHE_CONF said when we last looked - see load_config(). */
static char cfg_sys_env[VLHE_PATH_MAX];

/*
 * THE ORDINARY CHANNEL COUNT, a compile-time constant in the MODULE's
 * vsound_chan.h (VSOUND_MAX_CHAN, 4).
 *
 * RESTATED HERE RATHER THAN INCLUDED. That header carries the buffer
 * arithmetic as static helpers, so including it for one number costs
 * five "defined but not used" warnings in a file that is otherwise
 * clean - and it is a KERNEL-side header in a userspace program.
 *
 * The two cannot drift silently: vsound.h's VSOUND_LIST_CHAN is
 * VSOUND_MAX_CHAN + 1 for the reserved MIDI slot, and vsound_dev.c
 * checks those agree at compile time. The assertion below ties this
 * to that chain.
 */
#define VLHE_ORDINARY_CHANS  (VSOUND_LIST_CHAN - 1)

/* A build-time check, C89-style: a negative array size fails to
 * compile if VSOUND_LIST_CHAN ever stops being one more than four. */
typedef int vlhe_chan_count_check[(VLHE_ORDINARY_CHANS == 4) ? 1 : -1];

/*
 * WHICH /dev/dspN IS VSOUND - ASKED, NOT ASSUMED.
 *
 * THIS WAS HARDCODED "/dev/dsp" UNTIL 2026-09-25 AND IT IS WHY THE
 * VOLUME PAGE WENT BLANK. vsound takes the node the USER PICKED
 * (design/38), so on a machine where that pick is /dev/dsp2 every
 * caller here opened the real card instead: VSOUND_IOC_CHANS fails
 * against a card driver, chanlist() returns -1, vlhe_channels()
 * reports zero channels, and the page draws nothing. The same
 * hardcoded assumption had already been found and fixed in vmidid,
 * vdiscd and vsoundd's own input - this is the fourth instance.
 *
 * THE PROBE IS THE ONE THING THAT CANNOT BE STALE. A config value
 * says where vsound SHOULD be; the kernel says where it IS, and they
 * differ exactly when something went wrong - a half-failed load, a
 * hand `rmmod', a card that came back on a different node. design/38
 * section 7b has the case that settled it: with a two-DSP card
 * displaced, the arithmetic and the machine disagree.
 *
 * CACHED, AND THE STEADY STATE COSTS NOTHING EXTRA. vlhe_channels()
 * runs on the GUI's 250 ms poll and already opens one node to call
 * VSOUND_IOC_CHANS - which only vsound answers, so the call we were
 * making IS the identity check. A sweep happens only when the cache
 * is empty, and vsound_device_lost() empties it when that call
 * fails. The module's minor is fixed from insmod to rmmod, so a
 * cached answer cannot go stale while it is loaded.
 *
 * AND NOT AT ALL WITH NOTHING LOADED. Without the /proc/modules
 * check an idle GUI on an unloaded machine would sweep four nodes
 * every tick, failing each time - and esssolo1's open BLOCKS rather
 * than returning EBUSY (CLAUDE.md section 5), so a sweep across a
 * busy real card can stall the window. That is the one cost worth
 * avoiding, and it is confined to the path we have already lost
 * vsound on.
 */
static char vsound_dev_cache[VLHE_PATH_MAX];

static int
node_is_vsound(const char *node)
{
    struct vsound_stat st;
    struct stat ns;
    int fd, rc;

    /* LOOKING MUST NOT LOAD A MODULE - design/55 R11, design/54 D53 (2026-10-04). */
    if (stat(node, &ns) == 0 && S_ISCHR(ns.st_mode)
        && (int) major(ns.st_rdev) == 14
        && vlhe_modconf_open_would_load((int) minor(ns.st_rdev), NULL, 0))
        return 0;
    fd = open(node, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    rc = ioctl(fd, VSOUND_IOC_STAT, &st);
    close(fd);
    return rc == 0;
}

/* The cached node stopped answering - forget it, so the next call
 * sweeps. Called from the places that make the ioctl. */
/* THE LAST CHANNEL LIST, KEPT 100 ms - see chanlist(). Declared here
 * so the device-lost hook can drop it with the node. */
static struct vsound_chanlist g_cl;
static struct timeval         g_cl_when;
static int                    g_cl_valid;

static void
chanlist_forget(void)
{
    g_cl_valid = 0;
}

static void
vsound_device_lost(void)
{
    vsound_dev_cache[0] = '\0';
    chanlist_forget();
}

static const char *
vsound_device(void)
{
    const char *v = getenv("VLHE_DSP");
    int i;

    /* THE OVERRIDE STILL WINS, and it is how the host tests point
     * this at a fixture - they have no vsound to answer an ioctl. */
    if (v != NULL && *v != '\0')
        return v;

    if (vsound_dev_cache[0] != '\0')
        return vsound_dev_cache;

    /* NOTHING TO FIND IF IT IS NOT LOADED - see the header above. */
    if (!vlhe_status_module_loaded("vsound"))
        return "/dev/dsp";

    /*
     * ASK THE MODULE FIRST - /proc/vsound, design/38 9f2. It is the
     * only answer that works when the NODE IS MISSING, which is the
     * stock case: MAKEDEV makes two dsp nodes, an es1371 takes both,
     * and vsound ends up on a minor nothing can open. The sweep
     * below cannot see that and neither can any ioctl.
     *
     * The node may still not exist after this - reporting the index
     * does not create it - but the caller then knows what to make,
     * and the plan does (VLHE_STEP_NODE).
     */
    {
        int idx = vlhe_vsound_dsp();

        if (idx >= 0) {
            if (idx == 0)
                strcpy(vsound_dev_cache, "/dev/dsp");
            else
                sprintf(vsound_dev_cache, "/dev/dsp%d", idx);
            return vsound_dev_cache;
        }
    }

    /*
     * AND FALL BACK TO THE SWEEP for a module too old to publish it.
     * Anything built before 2026-09-25 has no /proc/vsound, and a
     * staged image can be older than the GUI reading it.
     */

    /* /dev/dsp FIRST, so the single-card case costs exactly one open
     * and behaves as it always did. */
    for (i = 0; i < VLHE_DSP_PROBE_MAX; i++) {
        char node[VLHE_PATH_MAX];

        if (i == 0)
            strcpy(node, "/dev/dsp");
        else
            sprintf(node, "/dev/dsp%d", i);

        if (node_is_vsound(node)) {
            strncpy(vsound_dev_cache, node, sizeof vsound_dev_cache - 1);
            vsound_dev_cache[sizeof vsound_dev_cache - 1] = '\0';
            return vsound_dev_cache;
        }
    }

    /* NOT FOUND. Answer /dev/dsp rather than nothing: every caller
     * opens it and fails, which is the same outcome as before and
     * keeps this function total. */
    return "/dev/dsp";
}

static void
load_config(void)
{
    const char *p;

    /*
     * AND THE OVERRIDES CAN MOVE UNDER US. VLHE_CONF and
     * VLHE_USER_CONF are how the host tests point this at a fixture,
     * and test_vlhe_commit changes VLHE_CONF MID-RUN to check that a
     * failed write is reported rather than swallowed. Caching the
     * paths without noticing that defeats it - four tests failed the
     * moment this function started remembering them.
     *
     * So a changed override forces a reload. It costs one getenv and
     * one strcmp per call, and it keeps the environment meaning what
     * it says everywhere, not only before the first read.
     */
    if (cfg_loaded) {
        const char *e = getenv("VLHE_CONF");

        if (e == NULL)
            e = "";
        if (strcmp(e, cfg_sys_env) != 0)
            cfg_loaded = 0;     /* it moved - read again */
    }

    if (cfg_loaded)
        return;
    cfg_loaded = 1;

    {
        const char *e = getenv("VLHE_CONF");

        strncpy(cfg_sys_env, e != NULL ? e : "",
                sizeof cfg_sys_env - 1);
        cfg_sys_env[sizeof cfg_sys_env - 1] = '\0';
    }

    /*
     * THE PATHS ARE DECIDED ONCE PER LOAD AND REMEMBERED - found on
     * 86Box 2026-09-22, the user: "a few bugs fighting over which
     * config file to use hitting okay and apply and save settings".
     *
     * THE RESOLVERS ARE NOT CONSTANT. Both answer partly from what
     * exists on disk - trial mode versus installed, and whether a
     * config is already there - so CREATING one mid-session changes
     * the answer. The reader cached its choice here at startup and
     * every writer called the resolver again, so after a Save the
     * two disagreed: reads came from the file chosen before it
     * existed, writes went to the one chosen after.
     *
     * Remembering both makes a session self-consistent. A machine
     * whose situation genuinely changed gets the new answer on the
     * next load, which vlhe_reset_settings() and the commit path
     * already force.
     */
    p = vlhe_conf_system_path();
    strncpy(cfg_sys_path, p != NULL ? p : "", sizeof cfg_sys_path - 1);
    /* fall through - the user path follows below */
    cfg_sys_path[sizeof cfg_sys_path - 1] = '\0';

    p = vlhe_conf_user_path();
    strncpy(cfg_usr_path, p != NULL ? p : "", sizeof cfg_usr_path - 1);
    cfg_usr_path[sizeof cfg_usr_path - 1] = '\0';

    /* A MISSING FILE IS AN EMPTY ONE, not an error - every key has a
     * default and a machine with no config must still work. */
    /*
     * JUDGED AS IT IS READ, AND AN UNTRUSTED FILE'S VALUES ARE NOT
     * LOADED - design/49 T0 and R1. The first version kept them "so
     * the pages show what the file says", and the review found three
     * consequences: root changing any other setting and saving wrote
     * the whole in-memory config as root, adopting the untrusted
     * values unreviewed; importing that same file with Load
     * Configuration compared against those values and found nothing
     * to change, so the route the refusal recommends did nothing;
     * and the verdict was never refreshed. So: the settings start as
     * the defaults, exactly as for a missing file, the verdict and
     * the owner's name are kept for the GUI to say, and the values
     * reach memory only through the review.
     *
     * AN UNREADABLE FILE IS UNTRUSTED TOO - R5. -1 used to mean "the
     * defaults, and say nothing"; a Load then proceeded on defaults
     * with no word that the config could not be read.
     */
    switch (g_conf_trust_all
            ? (vlhe_conf_read(&cfg_sys, cfg_sys_path) < 0 ? -1 : 0)
            : vlhe_conf_read_trusted(&cfg_sys, cfg_sys_path,
                                     cfg_sys_untrusted,
                                     (int) sizeof cfg_sys_untrusted)) {
    case 0:
        cfg_sys_untrusted[0] = '\0';
        break;
    case 1:
        cfg_sys.n = 0;                  /* read, judged, NOT loaded */
        cfg_sys.dirty = 0;
        break;
    default:
        cfg_sys.n = 0;
        if (cfg_sys_untrusted[0] == '\0')
            sprintf(cfg_sys_untrusted, "file %.200s cannot be read: %.60s",
                    cfg_sys_path, strerror(errno));
        break;
    }

    /*
     * NO SYSTEM FILE: MEMORY STARTS FROM THE SEED, not from the
     * compiled-in defaults - 2026-10-01, with Save-only (design/51).
     * The pages then show what the machine was installed as, which is
     * what the first-run Create used to put in the file before anyone
     * saw it; and the first Save writes those values rather than the
     * template's. Not dirty: nothing has been chosen yet. Only a seed
     * root could have written (design/49 T0).
     */
    if (cfg_sys.n == 0 && !file_exists(cfg_sys_path)
        && cfg_sys_untrusted[0] == '\0') {
        struct vlhe_conf seed;
        char sw[VLHE_PATH_MAX + 64];

        memset(&seed, 0, sizeof seed);
        if (seed_read_for(&seed, 1, sw, (int) sizeof sw)) {
            apply_seed(&cfg_sys, &seed, VLHE_TPL_SYSTEM, 0);
            cfg_sys.dirty = 0;
        }
    }

    if (cfg_usr_path[0] == '\0'
        || vlhe_conf_read(&cfg_usr, cfg_usr_path) < 0)
        cfg_usr.n = 0;

    /*
     * STRAY [Interface] KEYS UNDER [CDG Viewer] - 2026-10-01. Until
     * today the template listed StartPage, VerticalSliders and
     * ShowApplyResult AFTER the CDG Viewer block, and the writer
     * emits a section header only the first time it meets a section,
     * so every user file written since those keys existed carries
     * them under [CDG Viewer] - once per Save, because the reader
     * handed them back as that section's unknown keys and the writer
     * appended them again. Read as CDG Viewer keys, their readers
     * (which ask for [Interface]) never saw them: VerticalSliders=1
     * saved on every Save, faders horizontal on every start.
     *
     * Moved here, once, on read: the LAST stray in file order is the
     * newest - the template-driven line is written after the
     * appended ones - and becomes the [Interface] value unless the
     * file already has one there; every stray is then dropped, so the
     * next write is clean. The read is marked dirty only if something
     * moved, which is what vlhe_conf_set() and _unset() already do.
     */
    {
        static const char *keys[] = { "StartPage", "VerticalSliders",
                                      "ShowApplyResult" };
        int k;

        for (k = 0; k < 3; k++) {
            char last[VLHE_CONF_VAL_MAX];
            int  i, found = 0;

            for (i = 0; i < cfg_usr.n; i++)
                if (strcmp(cfg_usr.entry[i].section, "CDG Viewer") == 0
                    && strcmp(cfg_usr.entry[i].key, keys[k]) == 0) {
                    strncpy(last, cfg_usr.entry[i].value, sizeof last - 1);
                    last[sizeof last - 1] = '\0';
                    found = 1;
                }
            if (!found)
                continue;
            if (vlhe_conf_get(&cfg_usr, "Interface", keys[k], NULL) == NULL)
                vlhe_conf_set(&cfg_usr, "Interface", keys[k], last);
            while (vlhe_conf_unset(&cfg_usr, "CDG Viewer", keys[k]) == 0)
                ;
        }
    }

    /* A draft in use is laid over what was just read - see the
     * function. */
    readback_reapply();
}

/* ------------------------------------------------------------------ */
/* Committing - what makes a setting outlive the process              */
/* ------------------------------------------------------------------ */

/*
 * THE SEED - design/33 section 1b. Read, never merged: it replaces
 * where the DEFAULTS come from, and only when there is no system
 * config. One source or the other, so there is no precedence rule in
 * here and vlhe_conf_get() is untouched.
 */
static const char *
seed_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *v = getenv("VLHE_SEED");

    if (v != NULL && *v != '\0')
        return v;
    /*
     * A PORTABLE FOLDER'S OWN SEED FIRST - 2026-10-02, the user's
     * choice ("2 please it also tests the seed importing"): defaults.conf
     * beside the program, as the folder's vlhe.conf is. It lets a test
     * image carry its settings (mkxfer.sh -T stages one with tracing on)
     * without touching the guest's /etc, and a restage without -T takes
     * it away. A folder with none falls through to the machine's.
     */
    if (vlhe_self_is_trial()
        && vlhe_self_path("defaults.conf", buf, sizeof buf)
        && access(buf, F_OK) == 0)
        return buf;
    return "/etc/vlhe/defaults.conf";
}

/*
 * READ THE SEED FOR ONE FILE - design/49 T0, fix 1's follow-up.
 *
 * The system file is root's and root acts on it, so a seed going INTO
 * it must be one only root could have written - the same test
 * vlhe_conf_read_trusted() applies to the config. Otherwise a user's
 * seed would reach root through the create path instead of the Load
 * path. A seed that fails is not used for the system file, which gets
 * the template, and `why' says so. For a user file it is read plainly:
 * that file is the user's own. Returns 1 if the seed was applied.
 */
static int g_seed_trust_all;     /* the host tests only - see below */

void
vlhe_backend_set_seed_trusted(int on)
{
    g_seed_trust_all = on ? 1 : 0;
}

void
vlhe_backend_set_config_trusted(int on)
{
    g_conf_trust_all = on ? 1 : 0;
}

static int
seed_read_for(struct vlhe_conf *seed, int for_system, char *why, int max)
{
    int rc;

    if (why != NULL && max > 0)
        why[0] = '\0';
    if (!for_system || g_seed_trust_all)
        return vlhe_conf_read(seed, seed_path()) == 0 && seed->n > 0;

    rc = vlhe_conf_read_trusted(seed, seed_path(), why, max);
    if (rc != 0) {
        seed->n = 0;            /* read, judged, and not to be used */
        return 0;
    }
    return seed->n > 0;
}

int
vlhe_seed_exists(void)
{
    FILE *fp = fopen(seed_path(), "r");

    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

static int
file_exists(const char *path)
{
    FILE *fp;

    if (path == NULL)
        return 0;
    fp = fopen(path, "r");
    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

/* ONE FILE, NAMED - because "either exists" is the wrong question for
 * "should this one be created" (design/47 B1; the 2026-10-01 case). */
int
vlhe_conf_file_exists(int which)
{
    load_config();
    return file_exists(which == VLHE_TPL_SYSTEM ? cfg_sys_path
                                                : cfg_usr_path);
}

int
vlhe_conf_exists(void)
{
    /* EITHER file counts. A user who has never been root has only the
     * user file and their settings DO persist, so answering "no"
     * would prompt them to create something they already have. */
    load_config();      /* so the paths are decided before we look */
    return file_exists(cfg_sys_path) ||
           (cfg_usr_path[0] != '\0' && file_exists(cfg_usr_path));
}

/*
 * MAY ROOT ACT ON THE SYSTEM CONFIG? - design/49 T0.
 *
 * A normal user's Save created /mnt/xfer/vlhe.conf, owned by them, and
 * root's Load then read its module, option and daemon choices. Every
 * plan run asks this first. The verdict is the one reached when the
 * file was read - the in-memory copy is what a plan uses, and a file
 * that was root's in a directory only root can change cannot have
 * been replaced since.
 */
int
vlhe_conf_system_trusted(char *why, int max)
{
    load_config();
    if (cfg_sys_untrusted[0] == '\0')
        return 1;
    if (why != NULL && max > 0) {
        strncpy(why, cfg_sys_untrusted, (size_t) max - 1);
        why[max - 1] = '\0';
    }
    return 0;
}

int
vlhe_dirty(void)
{
    load_config();
    return (cfg_sys.dirty || cfg_usr.dirty) ? 1 : 0;
}

/* Fill in the failure, bounded. */
static void
commit_fail(struct vlhe_commit_err *err, const char *path, const char *why)
{
    if (err == NULL)
        return;
    if (path != NULL) {
        strncpy(err->path, path, sizeof err->path - 1);
        err->path[sizeof err->path - 1] = '\0';
    }
    if (why != NULL) {
        strncpy(err->why, why, sizeof err->why - 1);
        err->why[sizeof err->why - 1] = '\0';
    }
}

static int g_commit_user_only;      /* vlhe_commit_user() - skip /etc */

int
vlhe_user_dirty(void)
{
    load_config();
    return cfg_usr.dirty ? 1 : 0;
}

int
vlhe_commit_user(struct vlhe_commit_err *err)
{
    int rc;

    g_commit_user_only = 1;
    rc = vlhe_commit(err, 0);
    g_commit_user_only = 0;
    return rc;
}

int
vlhe_commit(struct vlhe_commit_err *err, int create)
{
    const char *p;
    int rc;

    if (err != NULL) {
        err->path[0] = '\0';
        err->why[0] = '\0';
        err->user_tier = -1;
        err->alt[0] = '\0';
        err->drafted[0] = '\0';
    }

    load_config();

    /*
     * ONLY WHAT CHANGED, AND THE BACKUP IS WHY. design/33 keeps one
     * `.old' per file, so writing an unchanged file copies it over
     * its own backup and the previous version is gone. A clean commit
     * writes NOTHING and succeeds.
     *
     * The two files hold disjoint keys (design/33 section 1), so
     * checking them separately is not an optimisation - it is what
     * stops a Sound Settings edit from rewriting the user's file.
     */
    if (cfg_sys.dirty && !g_commit_user_only) {
        p = cfg_sys_path;

        /*
         * A MISSING FILE IS NOT WRITTEN UNLESS ASKED. The settings
         * stay in memory and stay dirty, so a later Save picks them
         * up - nothing is lost by not writing, which is exactly what
         * makes a no-config session usable.
         */
        if (!create && !file_exists(p))
            goto user_file;

        /*
         * NOT WITHOUT ROOT, EVEN WHERE THE FILE IS WRITABLE - design/49
         * T0. The portable tree is 1777, so a user could write it, and
         * root's Load then acted on what they wrote.
         *
         * SO THE MACHINE SETTINGS GO TO THE DRAFT, AND THAT IS A SAVE
         * - the user's decision, 2026-10-01 (design/51). Only the File
         * menu writes files now, so a user who cannot write the system
         * file still needs Save to put their machine settings
         * somewhere that survives: the draft at its default path,
         * which the next start reads back (vlhe_conf_draft_readback())
         * and which root imports with Load Configuration. The dirty
         * bit CLEARS - a star the user can never clear teaches them to
         * ignore stars - and `err->drafted' names the file so the
         * caller can say where the settings went.
         *
         * It used to refuse here with "needs root ... kept, not
         * saved", which under OK-writes-too was a status; under
         * Save-only it would be a Save that saved nothing.
         */
        if (!root_available()) {
            const char *d = vlhe_draft_default_path();

            if (d == NULL || d[0] == '\0') {
                commit_fail(err, "vlhe-draft.conf", "needs root, and there"
                            " is nowhere to keep a draft - no home"
                            " directory");
                return -1;
            }
            if (vlhe_conf_template_write(&cfg_sys, d, VLHE_TPL_SYSTEM, 0)
                != 0) {
                commit_fail(err, d, strerror(errno));
                return -1;
            }
            cfg_sys.dirty = 0;
            g_draft_pending = 0;
            readback_note(d);
            if (err != NULL) {
                strncpy(err->drafted, d, sizeof err->drafted - 1);
                err->drafted[sizeof err->drafted - 1] = '\0';
            }
            goto user_file;
        }

        /*
         * THE FIRST WRITE CARRIES THE SEED - design/33 section 1b,
         * as vlhe_conf_create() always did: what this machine was
         * INSTALLED as fills every key the pages have not set, so a
         * Save with nothing touched on a page does not write the
         * compiled-in default over the installer's answer. Only a
         * seed root could have written (design/49 T0), and only when
         * the file does not exist yet.
         */
        if (!file_exists(p)) {
            struct vlhe_conf seed;
            char sw[VLHE_PATH_MAX + 64];

            memset(&seed, 0, sizeof seed);
            if (seed_read_for(&seed, 1, sw, (int) sizeof sw))
                apply_seed(&cfg_sys, &seed, VLHE_TPL_SYSTEM, 0);
        }

        /* THE SYSTEM FILE IS THE ONE WRITE HERE THAT NEEDS ROOT, so
         * it alone is raised. The user half below runs as the user,
         * which is what it wants - see the drop there. */
        vlhe_root_begin();
        rc = vlhe_conf_template_write(&cfg_sys, p, VLHE_TPL_SYSTEM, 1);
        if (rc == 0)
            system_file_claim(p);
        vlhe_root_end();                /* keeps errno */
        if (rc != 0) {
            /* THE LIKELY CAUSE IS NAMED RATHER THAN GUESSED AT. A
             * non-root user editing a machine setting is the common
             * case, and "Permission denied" alone leaves them
             * wondering what to do about it. */
            commit_fail(err, p,
                        (errno == EACCES || errno == EPERM)
                        ? "needs root - run the control centre as root"
                        : strerror(errno));
            return -1;
        }
        cfg_sys.dirty = 0;
        g_draft_pending = 0;            /* saved - R7 */
        /* THE DRAFT IN USE IS SUPERSEDED by this write - the system
         * file is now the newer, and the next start will say so. The
         * file itself is the user's and stays (design/51). */
        g_readback_path[0] = '\0';
        /* JUDGED AGAIN, NOW THAT ROOT HAS WRITTEN IT - R1. The verdict
         * was set only in load_config(), so after root's own copy went
         * down Load kept refusing until a restart. Nothing is loaded
         * here - memory already holds what was written. */
        {
            static struct vlhe_conf scratch;
            int rc2 = vlhe_conf_read_trusted(&scratch, p, cfg_sys_untrusted,
                                             (int) sizeof cfg_sys_untrusted);
            if (rc2 == 0)
                cfg_sys_untrusted[0] = '\0';
        }
    }

user_file:
    if (cfg_usr.dirty) {
        int rc, saved_errno;

        /*
         * THE USER FILE IS ALWAYS CREATED - THE `create' GATE WAS
         * NEVER MEANT FOR IT, 2026-09-24.
         *
         * The gate above is a CONSENT rule about /etc: pressing Apply
         * on a machine with no system config is not consent to start
         * writing there. The user file was swept into the same rule by
         * copy, and there is no /etc at stake in it - it holds the
         * user's own levels, recent list and font folders. So this
         * half used to read
         *
         *     if (!create && p != NULL && !file_exists(p))
         *         return 0;
         *
         * and a non-root user who had deleted root's vlhe-user.conf
         * pressed Apply, pressed OK, added a font folder - and every
         * one returned SUCCESS having written nothing, under a status
         * line saying the settings were saved. design/36 row 59.
         *
         * THE PATH IS RE-RESOLVED HERE rather than taken from the
         * cache, because vlhe_conf_user_redirect() may have moved it
         * since load_config() ran - that is the GUI's "write to $HOME
         * instead?" answer taking effect.
         *
         * AND THE ALWAYS-CREATE RULE ABOVE IS REVERSED, 2026-09-26 -
         * design/36 row 88. The paragraph above it is kept because
         * the failure it describes is real and must not come back.
         *
         * WHAT CHANGED IS A LATER DECISION OF THE USER'S, and the two
         * genuinely collided. 2026-09-24 said the `create' gate is a
         * consent rule about /etc alone, so the user file should be
         * written regardless. 2026-09-25 said Apply is memory-only on
         * BOTH halves - *"the user settings that do save on apply
         * SHOULD not be saved they should be cached like the other
         * settings"* - which is row 59's reversal and commit
         * `f7f06ab'. This half never followed.
         *
         * THE USER HIT IT ON 2026-09-26: pressing Apply on CD
         * Settings created `vlhe-user-root.conf' on a machine that
         * had none. Their call: *"I think A because it may be some
         * user settings we already store."*
         *
         * ROW 59 IS NOT REOPENED BY THIS, and that is the point worth
         * being careful about. Its bug was a SILENT no-op reporting
         * success. The GUI now tells the user when there is no file
         * to write to - *"Settings applied - not saved, there is no
         * configuration file"* - and offers to make one. So nothing
         * is dropped without saying so; the difference is that the
         * user decides, rather than Apply deciding for them.
         */
        p = vlhe_conf_user_path();
        if (p != NULL) {
            strncpy(cfg_usr_path, p, sizeof cfg_usr_path - 1);
            cfg_usr_path[sizeof cfg_usr_path - 1] = '\0';
        }

        /* MEMORY ONLY UNLESS ASKED - the same rule the system half
         * above applies. `create' is 1 from File / Save Configuration
         * and from the no-config dialog's yes; Apply and OK pass 0. */
        if (!create && cfg_usr_path[0] != '\0'
            && !file_exists(cfg_usr_path))
            return 0;

        if (p == NULL) {
            commit_fail(err, "~/.vlhe/vlhe.conf", "no home directory");
            if (err != NULL)
                err->user_tier = VLHE_USER_TIER_DEFAULT;
            return -1;
        }

        /*
         * A MISSING FILE IS NOT WRITTEN UNLESS ASKED - the same rule
         * the system half above has kept all along, and this half
         * did not.
         *
         * THE USER FOUND IT ON TARGET, 2026-09-25: "no settings were
         * saved so I got that message again but if I change user
         * settings that writes to the folder. it is inconsistant."
         * Exactly so. Apply on a machine with no config left the
         * system settings in memory and CREATED the user file, so
         * the no-config notice stayed up while a file had quietly
         * appeared beside the binary.
         *
         * AND THE FIX IS TO LEVEL DOWN, NOT UP - their call. Making
         * Apply create both files would remove the notice by making
         * the offer meaningless; caching both makes Apply mean one
         * thing everywhere, and `File / Save Configuration' stays
         * the single act that writes anything.
         *
         * NOTHING IS LOST. The settings stay in memory and stay
         * dirty, so the later Save picks them up - which is what the
         * system half's comment has always said.
         */
        if (!create && !file_exists(p))
            return 0;

        /*
         * NEVER WRITTEN AS ROOT FOR ANOTHER USER - refused, not
         * dropped around. Rewritten 2026-09-30 after a review of the
         * hardening.
         *
         * THIS USED TO DROP TO THE REAL USER AND REGAIN AFTERWARDS,
         * because the setuid build held euid 0 for the whole session
         * after Modify and a plain write would have left the user's
         * config root-owned. Since b48dcf7 euid is 0 only inside a
         * vlhe_root_begin() window, and nothing commits from inside
         * one, so that drop could no longer run.
         *
         * IT WAS REMOVED RATHER THAN LEFT DORMANT because it was a
         * SECOND, UNCHECKED IMPLEMENTATION of dropping privilege,
         * bypassing vlhe_priv.c's drop_or_die(): if it had ever failed
         * it would have written as root, and it would have come back
         * to life the moment a future change committed from inside a
         * raised window.
         *
         * SO THE CONDITION IT HANDLED IS NOW A REFUSAL. If this is
         * reached with euid 0 while the real user is not root, the
         * model has been broken somewhere; a failed save says so,
         * where silently writing the user's file as root would not.
         * The backend cannot call drop_or_die() - the CLI and the host
         * tests link it without vlhe_priv.c - which is another reason
         * to refuse rather than re-implement the drop.
         *
         * Real root (getuid() == 0) writes its own file as ever.
         */
        if (geteuid() == 0 && getuid() != 0) {
            commit_fail(err, p, "refused: the user settings would be"
                                " written as root - this is a bug,"
                                " please report it");
            return -1;
        }

        /* NO BACKUP ON THE USER FILE - design/33 section 3. It holds
         * levels and a recent list; a stale `.old' beside it would be
         * clutter rather than insurance. */
        rc = vlhe_conf_template_write(&cfg_usr, p, VLHE_TPL_USER, 0);
        saved_errno = errno;

        if (rc != 0) {
            commit_fail(err, p, strerror(saved_errno));
            /*
             * AND SAY WHERE ELSE THEY COULD GO. The tier that failed
             * and the next candidate, so the GUI can ASK about $HOME
             * and NOTIFY about /tmp (design/36 row 60), and the CLI
             * can name them and stop.
             */
            if (err != NULL) {
                int tier = vlhe_conf_user_tier();
                const char *alt = NULL;

                err->user_tier = tier;
                if (tier == VLHE_USER_TIER_DEFAULT) {
                    alt = vlhe_conf_user_candidate(VLHE_USER_TIER_HOME);
                    /* installed: DEFAULT already IS $HOME - offer /tmp */
                    if (alt != NULL && strcmp(alt, p) == 0)
                        alt = vlhe_conf_user_candidate(VLHE_USER_TIER_TMP);
                } else if (tier == VLHE_USER_TIER_HOME) {
                    alt = vlhe_conf_user_candidate(VLHE_USER_TIER_TMP);
                }
                if (alt != NULL) {
                    strncpy(err->alt, alt, sizeof err->alt - 1);
                    err->alt[sizeof err->alt - 1] = '\0';
                }
            }
            return -1;
        }
        cfg_usr.dirty = 0;
    }

    return 0;
}

/*
 * THE SEED OVER THE TEMPLATE, KEY BY KEY.
 *
 * This is NOT the per-key layering design/33 section 1b rejected -
 * that was about resolving every READ across several files forever.
 * This runs ONCE, at creation, to build the file that is then read
 * normally. After this returns there is one config file and no
 * precedence anywhere.
 *
 * Copying key by key rather than using the seed wholesale is what
 * makes a PARTIAL seed valid: a seed naming three keys contributes
 * three, and the template supplies the rest.
 */
static void
apply_seed(struct vlhe_conf *c, const struct vlhe_conf *seed, int which,
           int overwrite)
{
    static struct vlhe_conf tpl;        /* large: not on the stack */
    int i;

    /*
     * WHICH KEYS BELONG IN THIS FILE IS THE TEMPLATE'S ANSWER - fixed
     * 2026-10-02. The seed is one flat file describing the machine, and
     * the template knows which keys go in the system file and which in
     * the user file; a seed key in neither is from a newer setup and is
     * skipped.
     *
     * THIS USED TO ASK `c' INSTEAD - "only keys this file already has"
     * - and that was right only for the two create paths, where `c'
     * starts as the full template. At load with no system file `c' is
     * EMPTY, so nothing was ever imported: a config-less load ignored
     * the seed entirely (a host test, the day a portable test seed was
     * built to rely on it). And at the first Save `c' holds exactly the
     * keys the pages CHANGED, so the seed overwrote the user's edits
     * and filled nothing - the reverse of what that caller's comment
     * says it does.
     *
     * `overwrite': 1 where `c' starts from the template's defaults (the
     * create paths - the seed's answer replaces the default); 0 where
     * `c' holds choices (load, the first Save - fill only what is not
     * already there, so an edit is never undone by the seed).
     */
    memset(&tpl, 0, sizeof tpl);
    vlhe_conf_template_defaults(&tpl, which);

    for (i = 0; i < seed->n; i++) {
        if (vlhe_conf_get(&tpl, seed->entry[i].section,
                          seed->entry[i].key, NULL) == NULL)
            continue;                   /* not this file's key */
        if (!overwrite && vlhe_conf_get(c, seed->entry[i].section,
                                        seed->entry[i].key, NULL) != NULL)
            continue;                   /* a choice already made */
        vlhe_conf_set(c, seed->entry[i].section, seed->entry[i].key,
                      seed->entry[i].value);
    }
}

const char *
vlhe_system_conf_path(void)
{
    load_config();
    return cfg_sys_path;
}

const char *
vlhe_user_conf_path(void)
{
    load_config();
    return cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
}

/*
 * COULD THIS SESSION SAVE A MACHINE SETTING? ROOT FIRST, THEN THE
 * FILE - design/49 T0. Writability alone said yes to any user in the
 * 1777 portable tree. access() tests the REAL uid, so it is asked only
 * when the real uid is root; in the setuid build after Modify it
 * would answer for the user, not for us.
 */
static int
system_conf_savable(void)
{
    load_config();
    if (!root_available())
        return 0;
    return getuid() != 0 || vlhe_conf_writable(cfg_sys_path);
}

int
vlhe_can_write_system_conf(void)
{
    return system_conf_savable();
}

void
vlhe_discard(void)
{
    /* RE-READ IS THE DISCARD. The dirty flag is cleared by
     * vlhe_conf_read() itself, so there is nothing else to undo -
     * every unsaved set() lived only in these two structs. */
    cfg_loaded = 0;
    g_draft_pending = 0;                /* discarded - R7 */
    load_config();
    /* AND THE DRAFT IN USE COMES BACK WITH THE FILES - design/51. For
     * a user whose machine settings live in the draft, "undo what was
     * never saved" means back to the draft, not to a system file they
     * were never looking at. */
    readback_reapply();
}

/*
 * THE DRAFT IN USE COMES BACK WHENEVER THE FILES ARE RE-READ - not only
 * on Cancel. vlhe_conf_create() and the reset and restore paths all
 * drop cfg_loaded and read again, and until 2026-10-01 only
 * vlhe_discard() re-applied the draft: a Save by a locked user whose
 * machine half had just gone to the draft, followed by the creation
 * of a still-absent user file, re-read the (absent) system file over
 * memory and the drafted settings vanished from the pages. Called at
 * the end of load_config(), guarded against its own recursion (the
 * review inside calls load_config(), which returns at once with
 * cfg_loaded set).
 */
/*
 * IS `a' AT LEAST AS RECENT AS `b'? - design/54 D62, 2026-10-04.
 * Compared UNSIGNED. ext2 stores a time as an unsigned 32-bit count, and
 * the target's time_t is a signed 32-bit long, so from 19 January 2038
 * a fresh file's st_mtime reads NEGATIVE: a signed `>=' would call a
 * system file saved in 2037 newer than a draft saved in 2039, and drop
 * the draft. Unsigned keeps the order right until 2106 (design/50).
 */
static int
mtime_not_older(time_t a, time_t b)
{
    return (unsigned long) a >= (unsigned long) b;
}

static void
readback_reapply(void)
{
    struct stat ds, ss;
    static int in_reapply;

    if (g_readback_path[0] == '\0' || in_reapply)
        return;
    in_reapply = 1;
    /* NEWEST WINS, asked again: root may have saved since. */
    if (stat(g_readback_path, &ds) != 0
        || (file_exists(cfg_sys_path) && stat(cfg_sys_path, &ss) == 0
            && mtime_not_older(ss.st_mtime, ds.st_mtime)))
        g_readback_path[0] = '\0';
    else
        (void) readback_apply(g_readback_path, NULL, 0);
    in_reapply = 0;
}

/* ------------------------------------------------------------------ */
/* A draft, and loading one - design/49 T0                             */
/* ------------------------------------------------------------------ */

/*
 * THE TWO HALVES OF HANDING SETTINGS TO ROOT. A normal user cannot
 * write the system config (fix 2), and root must not act on a file a
 * user wrote (fix 1). So the user SAVES A DRAFT - the machine settings
 * on their screen, in a file of their own that Load never reads - and
 * root LOADS IT: every value is checked against what the pages offer,
 * set through the same setters, and left as unsaved edits for root to
 * look at and Save as root's own file. The draft is never trusted and
 * never taken over.
 */

/* The same file, however it is spelled? Both must exist to compare. */
static int
same_file(const char *a, const char *b)
{
    struct stat sa, sb;

    if (a == NULL || b == NULL || stat(a, &sa) != 0 || stat(b, &sb) != 0)
        return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/*
 * WHERE A DRAFT LIVES BY DEFAULT: beside the user's own file, or in
 * their home - somewhere that is theirs. The GUI's Save Draft opened
 * here from the start; since Save-only (design/51) it is also where
 * a non-root Save puts the machine settings, and what the next start
 * reads back.
 */
const char *
vlhe_draft_default_path(void)
{
    static char out[VLHE_PATH_MAX];
    const char *u = vlhe_conf_user_path();
    const char *h = getenv("HOME");
    char *slash;

    out[0] = '\0';
    if (u != NULL && (int) strlen(u) + 20 < (int) sizeof out) {
        strcpy(out, u);
        slash = strrchr(out, '/');
        if (slash != NULL)
            slash[1] = '\0';
        else
            out[0] = '\0';
    }
    if (out[0] == '\0' && h != NULL && (int) strlen(h) + 20 < (int) sizeof out)
        sprintf(out, "%s/", h);
    if (out[0] != '\0' && (int) strlen(out) + 17 < (int) sizeof out)
        strcat(out, "vlhe-draft.conf");
    return out;
}

/*
 * THE DRAFT READ BACK AT START - design/51, the user's decision
 * 2026-10-01: for a user who cannot write the system file, the draft
 * at its default path IS their machine configuration, so it has to
 * come back next time or the Save that wrote it was a lie.
 *
 * NEWEST WINS. The system file is root's and the draft is the user's;
 * whichever was written last is what the user last saw and meant. A
 * draft older than the system file is SUPERSEDED - root has saved
 * since - and is left where it is (the user's file, not ours to
 * delete) and reported once. A draft newer than the system file, or
 * with no system file at all, is read through the same review the
 * Load Configuration import uses - every key through its page's
 * setter, the one definition of valid - and installed NOT DIRTY:
 * these are saved settings, in the only place this user can save
 * them.
 *
 * ROOT NEVER READS ONE BACK. Root's Save writes the system file; a
 * draft at root's default path is an export (Save Draft...), and
 * reading it back would let an old export override what root just
 * saved.
 *
 * Returns 1 and the note when a draft was taken, -1 and the note when
 * one was found and not taken, 0 when there is none. `note' is a
 * sentence for the status line or the sidebar.
 */
static void
readback_note(const char *path)
{
    struct stat sb;

    strncpy(g_readback_path, path, sizeof g_readback_path - 1);
    g_readback_path[sizeof g_readback_path - 1] = '\0';
    g_readback_when = (stat(path, &sb) == 0) ? sb.st_mtime : 0;
}

static int
readback_apply(const char *path, char *note, int max)
{
    struct vlhe_draft_review r;
    char why[200];

    if (vlhe_conf_draft_review(path, &r, why, (int) sizeof why) != 0
        || vlhe_conf_draft_accept() != 0) {
        if (note != NULL && max > 0)
            sprintf(note, "Your draft %.*s was not read back: %.*s",
                    max > 240 ? 120 : max / 3, path,
                    max > 240 ? 100 : max / 3, why);
        g_readback_path[0] = '\0';    /* gone, or refused - not in use */
        return -1;
    }
    cfg_sys.dirty = 0;
    g_draft_pending = 0;
    readback_note(path);
    return 1;
}

int
vlhe_conf_draft_readback(char *note, int max)
{
    const char *d;
    struct stat ds, ss;

    if (note != NULL && max > 0)
        note[0] = '\0';
    load_config();
    if (root_available())
        return 0;
    d = vlhe_draft_default_path();
    if (d == NULL || d[0] == '\0' || stat(d, &ds) != 0)
        return 0;
    if (file_exists(cfg_sys_path) && stat(cfg_sys_path, &ss) == 0
        && mtime_not_older(ss.st_mtime, ds.st_mtime)) {
        if (note != NULL && max > 0)
            sprintf(note, "Your draft %.*s is older than the system"
                          " settings and was not used", max > 200 ? 120 : max / 2, d);
        return -1;
    }
    if (readback_apply(d, note, max) < 0)
        return -1;
    if (note != NULL && max > 0) {
        char when[40];
        struct tm *tm = localtime(&g_readback_when);

        when[0] = '\0';
        if (tm != NULL)
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", tm);
        sprintf(note, "Machine settings are from your draft of %s - the"
                      " system runs its own until root imports it",
                when);
    }
    return 1;
}

int
vlhe_conf_draft_in_use(char *path, int max, char *when, int wmax)
{
    if (g_readback_path[0] == '\0')
        return 0;
    if (path != NULL && max > 0) {
        strncpy(path, g_readback_path, max - 1);
        path[max - 1] = '\0';
    }
    if (when != NULL && wmax > 0) {
        struct tm *tm = localtime(&g_readback_when);

        when[0] = '\0';
        if (tm != NULL)
            strftime(when, wmax, "%Y-%m-%d %H:%M", tm);
    }
    return 1;
}

int
vlhe_conf_save_draft(const char *path, struct vlhe_commit_err *err)
{
    char dir[VLHE_PATH_MAX];
    const char *base;
    const char *sys_base;
    char *slash;

    if (err != NULL) {
        err->path[0] = '\0';
        err->why[0] = '\0';
        err->user_tier = -1;
        err->alt[0] = '\0';
    }
    load_config();
    if (path == NULL || *path == '\0') {
        commit_fail(err, "", "no file named");
        return -1;
    }

    /*
     * NEVER THE LIVE CONFIG OR THE SEED. A draft written over
     * vlhe.conf in the portable tree would be T0 again by another
     * name - a user-written file where root reads its settings. By
     * inode if it exists, by name in the same directory if not.
     */
    if (same_file(path, cfg_sys_path) || same_file(path, seed_path())) {
        commit_fail(err, path, "that is the live configuration - save"
                                " the draft under another name");
        return -1;
    }
    strncpy(dir, path, sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    slash = strrchr(dir, '/');
    base = (slash != NULL) ? slash + 1 : dir;
    sys_base = strrchr(cfg_sys_path, '/');
    sys_base = (sys_base != NULL) ? sys_base + 1 : cfg_sys_path;
    if (strcmp(base, sys_base) == 0) {
        commit_fail(err, path, "that is the live configuration's name -"
                                " save the draft under another name");
        return -1;
    }

    /* AS WHOEVER IS RUNNING - no raise. The draft is the user's file. */
    if (vlhe_conf_template_write(&cfg_sys, path, VLHE_TPL_SYSTEM, 0) != 0) {
        commit_fail(err, path, strerror(errno));
        return -1;
    }
    return 0;
}

/* A whole decimal number and nothing else - no "12abc", no "". */
static int
strict_int(const char *s, int *out)
{
    long v = 0;
    int neg = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (*s < '0' || *s > '9')
        return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 100000L)
            return 0;
        s++;
    }
    if (*s != '\0')
        return 0;
    *out = neg ? (int) -v : (int) v;
    return 1;
}

/* "" or /dev/dsp with up to three digits - what the Sound page's two
 * device lists can hold. */
static int
dsp_node_ok(const char *v)
{
    int n = 0;

    if (*v == '\0')
        return 1;
    if (strncmp(v, "/dev/dsp", 8) != 0)
        return 0;
    for (v += 8; *v != '\0'; v++, n++)
        if (*v < '0' || *v > '9' || n >= 3)
            return 0;
    return 1;
}

/*
 * ONE KEY FROM A DRAFT, THROUGH ITS PAGE'S SETTER - design/49 T0.
 *
 * The setters are the single definition of a valid setting (the
 * audit's point 1): each value is set on its own, into a struct read
 * from the getter, so a refusal names exactly the key that caused it
 * and nothing is clamped. Returns 1 taken, 0 refused, -2 the page's CURRENT
 * settings invalid (R8), -1 not a key
 * Load Configuration takes at all.
 *
 * NO PATHS. The keys that name a file root will read or run -
 * SoundFonts, [Paths] ModuleDir, the daemons, LAME - and CardModule,
 * which names a module to load, are simply not here: a range check
 * cannot make a path safe (the audit's point 2).
 */
static int
draft_key(const char *sec, const char *key, const char *val)
{
    int v = 0, k;
    int num = strict_int(val, &v);

    for (k = 0; k < 3; k++)
        if (strcmp(sec, enable_section[k]) == 0
            && strcmp(key, "LoadAtBoot") == 0) {
            if (!num || (v != 0 && v != 1))
                return 0;
            vlhe_set_component_enabled(k, v);
            return 1;
        }

    if (strcmp(sec, "CD Settings") == 0) {
        struct vlhe_modopts m;

        memset(&m, 0, sizeof m);
        (void) vlhe_modopts(&m);
        /* THE CURRENT SETTINGS FIRST - design/49 R8. If they do not
         * pass their own setter (a hand-edited file), every imported
         * key here would be refused under ITS name; -2 names the real
         * cause once. A no-op set: unchanged values dirty nothing. */
        if (vlhe_set_modopts(&m) != 0)
            return -2;
        if (strcmp(key, "Major") == 0)          m.major = v;
        else if (strcmp(key, "Drives") == 0)    m.ndevs = v;
        else if (strcmp(key, "Packet") == 0)    m.packet = v;
        else if (strcmp(key, "LinkCdrom") == 0) m.link_cdrom = v;
        else return -1;
        return num && vlhe_set_modopts(&m) == 0;
    }

    if (strcmp(sec, "Sound Settings") == 0) {
        struct vlhe_sound snd;

        if (strcmp(key, "SaveMixerLevels") == 0)
            return num && vlhe_mixer_set_restore(v) == 0;
        if (strcmp(key, "SaveProgramLevels") == 0)
            return num && vlhe_progvol_set_enabled(v) == 0;
        memset(&snd, 0, sizeof snd);
        (void) vlhe_sound(&snd);
        if (vlhe_set_sound(&snd) != 0)
            return -2;                  /* R8, as above */
        if (strcmp(key, "CardDevice") == 0
            || strcmp(key, "ProgramsUse") == 0) {
            char *dst = (key[0] == 'C') ? snd.card : snd.programs_use;

            if (strlen(val) >= VLHE_PATH_MAX)
                return 0;
            strcpy(dst, val);
            return vlhe_set_sound(&snd) == 0;
        }
        if (strcmp(key, "ReleaseOnIdle") == 0)    snd.release_on_idle = v;
        else if (strcmp(key, "MidiChannel") == 0) snd.midi_slot = v;
        else if (strcmp(key, "Limiter") == 0)     snd.limiter = v;
        else if (strcmp(key, "Attenuation") == 0) snd.attenuation = v;
        else return -1;
        return num && vlhe_set_sound(&snd) == 0;
    }

    if (strcmp(sec, "Midi Settings") == 0) {
        struct vlhe_synth    sy;
        struct vlhe_midiopts mo;

        if (strcmp(key, "Minor") == 0) {
            memset(&mo, 0, sizeof mo);
            (void) vlhe_midiopts(&mo);
            mo.minor = v;
            return num && vlhe_set_midiopts(&mo) == 0;
        }
        memset(&sy, 0, sizeof sy);
        (void) vlhe_synth(&sy);
        if (vlhe_set_synth(&sy) != 0)
            return -2;                  /* R8, as above */
        if (strcmp(key, "Voices") == 0)        sy.voices = v;
        else if (strcmp(key, "Gain") == 0)     sy.gain_milli = v;
        else if (strcmp(key, "Reverb") == 0)   sy.reverb = v;
        else if (strcmp(key, "Chorus") == 0)   sy.chorus = v;
        else if (strcmp(key, "Law") == 0)      sy.law = v;
        else if (strcmp(key, "Filter") == 0)   sy.filter = v;
        else if (strcmp(key, "Rate") == 0)     sy.rate = v;
        else if (strcmp(key, "AutoVoices") == 0) sy.auto_voices = v;
        else return -1;
        return num && vlhe_set_synth(&sy) == 0;
    }
    return -1;
}

/*
 * THE REVIEW, AND THE RESULT IT DESCRIBES. Built with the live
 * settings saved aside and put back, so nothing changes until Accept
 * installs `g_draft_after' - the audit's point 3.
 */
static struct vlhe_conf g_draft_after;
static int              g_draft_ready;

int
vlhe_conf_draft_review(const char *path, struct vlhe_draft_review *r,
                       char *why, int max)
{
    static struct vlhe_conf d, before, tpl_sys, tpl_usr;
    int i;

    memset(r, 0, sizeof *r);
    g_draft_ready = 0;
    if (why != NULL && max > 0)
        why[0] = '\0';
    load_config();

    /* NO UNSAVED MACHINE SETTINGS - the audit's point 4, narrowed by
     * design/49 T4. Otherwise the list would mix root's own pending
     * edits with the draft's. THE SYSTEM HALF ONLY: the import never
     * touches the user half, so a pending font or folder cannot mix
     * in - and on the first target test it did block, because Save
     * Draft does not save user settings and one was still pending. */
    if (cfg_sys.dirty) {
        if (why != NULL && max > 0) {
            strncpy(why, "there are unsaved machine settings - save or"
                         " cancel them first", max - 1);
            why[max - 1] = '\0';
        }
        return -1;
    }
    if (vlhe_conf_read_regular(&d, path) != 0) {
        if (why != NULL && max > 0) {
            strncpy(why, errno == EINVAL ? "not a regular file"
                                         : strerror(errno), max - 1);
            why[max - 1] = '\0';
        }
        return -1;
    }

    memset(&tpl_sys, 0, sizeof tpl_sys);
    memset(&tpl_usr, 0, sizeof tpl_usr);
    vlhe_conf_template_defaults(&tpl_sys, VLHE_TPL_SYSTEM);
    vlhe_conf_template_defaults(&tpl_usr, VLHE_TPL_USER);

    before = cfg_sys;
    for (i = 0; i < d.n; i++) {
        const char *sec = d.entry[i].section;
        const char *key = d.entry[i].key;
        int rc = draft_key(sec, key, d.entry[i].value);

        if (rc == -2) {
            /* THE PAGE'S CURRENT SETTINGS ARE INVALID - R8 - said once
             * per section, naming the cause rather than the key. */
            int k, seen = 0;

            for (k = 0; k < r->nrefused && k < VLHE_DRAFT_MAX; k++)
                if (strncmp(r->refused[k], sec, strlen(sec)) == 0
                    && strstr(r->refused[k], "current settings") != NULL)
                    seen = 1;
            if (!seen) {
                if (r->nrefused < VLHE_DRAFT_MAX)
                    sprintf(r->refused[r->nrefused], "%.40s - this page's"
                            " current settings are not valid, so nothing"
                            " from it was taken", sec);
                r->nrefused++;
            }
        } else if (rc == 0) {
            if (r->nrefused < VLHE_DRAFT_MAX)
                sprintf(r->refused[r->nrefused], "%.40s / %.24s = %.60s",
                        sec, key, d.entry[i].value);
            r->nrefused++;
        } else if (rc < 0) {
            /* THE USER'S OWN HALF IS COUNTED, NOT LISTED - the
             * audit's point 5: fonts, recent images and folders are
             * that user's and have no place in root's config. */
            if (vlhe_conf_get(&tpl_usr, sec, key, NULL) != NULL) {
                r->nuser++;
            } else {
                if (r->nskipped < VLHE_DRAFT_MAX)
                    sprintf(r->skipped[r->nskipped], "%.40s / %.40s",
                            sec, key);
                r->nskipped++;
            }
        }
    }
    g_draft_after = cfg_sys;
    cfg_sys = before;                   /* nothing has changed yet */

    /* WHAT WOULD MOVE: every key whose value differs from what it is
     * now - the file's, or the template's default if absent. */
    for (i = 0; i < g_draft_after.n; i++) {
        const struct vlhe_conf_entry *e = &g_draft_after.entry[i];
        const char *old = vlhe_conf_get(&before, e->section, e->key, NULL);

        if (old == NULL)
            old = vlhe_conf_get(&tpl_sys, e->section, e->key, "(unset)");
        if (strcmp(old, e->value) == 0)
            continue;
        /* TAB-SEPARATED, section, key, old, new - the GUI groups by
         * page and labels the key; a test greps the pieces. */
        if (r->nchange < VLHE_DRAFT_MAX)
            sprintf(r->change[r->nchange], "%.40s\t%.24s\t%.40s\t%.40s",
                    e->section, e->key, old, e->value);
        r->nchange++;
    }
    g_draft_ready = 1;
    return 0;
}

int
vlhe_conf_draft_accept(void)
{
    if (!g_draft_ready)
        return -1;
    g_draft_ready = 0;
    /* AS UNSAVED EDITS - dirty exactly when something moved, and Save
     * is still the only thing that writes. */
    cfg_sys = g_draft_after;
    g_draft_pending = cfg_sys.dirty ? 1 : 0;
    return 0;
}

/*
 * IS AN ACCEPTED DRAFT STILL UNSAVED? - design/49 R7. The GUI's Load
 * asked "backend dirty with no page dirty?" to detect an import and
 * got every other way the backend can be dirty with it: a Status-page
 * click whose system half could not be written (no config yet, or
 * not root) leaves those settings in memory and dirty by design, and
 * every Load then asked about "settings loaded from a file". This is
 * the flag the question should have been.
 */
int
vlhe_conf_draft_pending(void)
{
    return g_draft_pending;
}

void
vlhe_conf_draft_cancel(void)
{
    g_draft_ready = 0;
}

int
vlhe_conf_create(struct vlhe_commit_err *err)
{
    int rc;
    struct vlhe_conf seed;
    struct vlhe_conf c;
    const char *p;
    int written = 0;
    int have_seed;

    if (err != NULL) {
        err->path[0] = '\0';
        err->why[0] = '\0';
    }

    /*
     * WHERE THE DEFAULTS COME FROM - design/33 section 1b. The seed
     * if setup wrote one, the compiled-in template otherwise. READ,
     * NEVER MERGED, so this is a choice of source and not a layering.
     *
     * A PARTIAL SEED IS VALID, NOT CORRUPT. It may name three keys;
     * everything it does not mention is left to the template, which
     * is why the template is loaded FIRST and the seed applied over
     * it rather than replacing it wholesale.
     */
    memset(&seed, 0, sizeof seed);
    have_seed = 0;

    /* The system file. */
    p = cfg_sys_path;
    if (!file_exists(p) && !root_available()) {
        /* NOT WITHOUT ROOT, EVEN WHERE IT COULD BE WRITTEN - design/49
         * T0. The user file below is still made. */
        commit_fail(err, p, "needs root - the system file was not"
                            " created");
        if (err != NULL)
            err->needs_root = 1;
    } else if (!file_exists(p)) {
        char sw[VLHE_PATH_MAX + 64];

        memset(&c, 0, sizeof c);
        vlhe_conf_template_defaults(&c, VLHE_TPL_SYSTEM);
        have_seed = seed_read_for(&seed, 1, sw, (int) sizeof sw);
        if (have_seed)
            apply_seed(&c, &seed, VLHE_TPL_SYSTEM, 1);
        else if (sw[0] != '\0')
            /* SAID, NOT A FAILURE - the file is still made, from the
             * template. The GUI shows err only when nothing was. */
            commit_fail(err, seed_path(), "the seed was not used - a"
                        " user other than root could have written it");

        /* THE SYSTEM FILE NEEDS ROOT; the user file below does not
         * and is written lowered, as its owner - design/49 N9. */
        vlhe_root_begin();
        rc = vlhe_conf_template_write(&c, p, VLHE_TPL_SYSTEM, 0);
        if (rc == 0)
            system_file_claim(p);
        vlhe_root_end();                /* keeps errno */
        if (rc != 0) {
            int denied = (errno == EACCES || errno == EPERM);

            /* NOT A FAILURE OF THE OPERATION - a fact to show them.
             * An ordinary user cannot write /etc, and the user file
             * below is still worth creating, so this records the
             * reason and carries on. */
            commit_fail(err, p,
                        denied
                        ? "needs root - the system file was not created"
                        : strerror(errno));
            if (err != NULL && denied)
                err->needs_root = 1;
        } else {
            written++;
            /* JUDGED AGAIN - R1, as in vlhe_commit(). */
            {
                static struct vlhe_conf scratch;
                if (vlhe_conf_read_trusted(&scratch, p, cfg_sys_untrusted,
                                           (int) sizeof cfg_sys_untrusted)
                    == 0)
                    cfg_sys_untrusted[0] = '\0';
            }
        }
    }

    /* The user file. */
    p = cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
    if (p != NULL && !file_exists(p)) {
        memset(&c, 0, sizeof c);
        vlhe_conf_template_defaults(&c, VLHE_TPL_USER);
        /* READ AGAIN, PLAINLY - the user's own file may take a seed
         * the system file was not allowed to. */
        if (seed_read_for(&seed, 0, NULL, 0))
            apply_seed(&c, &seed, VLHE_TPL_USER, 1);

        if (vlhe_conf_template_write(&c, p, VLHE_TPL_USER, 0) != 0) {
            commit_fail(err, p, strerror(errno));
            return written > 0 ? written : -1;
        }
        written++;
    }

    /* Re-read, so the GUI shows what is now on disk rather than what
     * it had in memory before. */
    cfg_loaded = 0;
    load_config();

    return written;
}

/* ------------------------------------------------------------------ */
/* Channels - the Volume module                                       */
/* ------------------------------------------------------------------ */

/*
 * ONE IOCTL ANSWERS THE WHOLE MODULE. VSOUND_IOC_CHANS returns the
 * generation, the count and every slot in one call - which is what
 * makes a 250 ms poll cheap (vsound.h, and design/07 section 3.14 for
 * why that interval).
 *
 * Opened and closed per call rather than held: the GUI is not the pump
 * and must never be the thing keeping /dev/dsp open. vsound allows
 * many readers of the channel list; only the PUMP is exclusive.
 */
static int
chanlist(struct vsound_chanlist *cl)
{
    int fd, rc;

    /*
     * NO OPEN AT ALL WHILE THE MODULE IS ABSENT - 2026-09-30.
     *
     * vsound_device() answers "/dev/dsp" when vsound is not loaded,
     * and this poll then opened it four times a second. On a machine
     * where /dev/dsp is a link to a minor nothing serves - the 86Box
     * guest after a restage, /dev/dsp -> /dev/dsp1 with vsound not
     * yet loaded - every one of those opens made the kernel fork
     * modprobe twice ("can't locate module sound-slot-1",
     * sound_core.c:366): eight processes a second, 722 lines of
     * daemon.info in ninety seconds, for as long as the Volume page
     * was up (tests/logs/2026-09-30-86box-guest-var-log-kern). And
     * where /dev/dsp IS served, by the real card, the open reached
     * the card instead - harmless with O_NONBLOCK, pointless anyway.
     * /proc/modules is a read, costs no fork, and is the question
     * actually being asked.
     */
    if (!vlhe_status_module_loaded("vsound")) {
        chanlist_forget();
        return -1;
    }

    /*
     * ONE QUERY SERVES A WHOLE POLL - 2026-10-02. The Volume page asks
     * vlhe_sound_present(), vlhe_generation() and, when that moved,
     * vlhe_channels() - each of which opened vsound, so a page sitting
     * there opened it eight times a second, and each open printed a
     * trace line (tests/logs/2026-10-02-86box-guest-var-log: 1410 of
     * 2212). Kept 100 ms - under the 250 ms poll, so every poll asks
     * afresh once, and the three questions in it get one consistent
     * answer. A volume or mute change forgets it, so the next poll
     * sees the change at once.
     */
    if (g_cl_valid) {
        struct timeval now;
        long ms;

        gettimeofday(&now, NULL);
        ms = (now.tv_sec - g_cl_when.tv_sec) * 1000L
             + (now.tv_usec - g_cl_when.tv_usec) / 1000L;
        if (ms >= 0 && ms < 100) {
            *cl = g_cl;
            return 0;
        }
    }

    fd = open(vsound_device(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        /* The node we thought was ours will not even open. */
        vsound_device_lost();
        return -1;
    }
    rc = ioctl(fd, VSOUND_IOC_CHANS, cl);
    close(fd);
    if (rc == 0) {
        g_cl = *cl;
        gettimeofday(&g_cl_when, NULL);
        g_cl_valid = 1;
    }
    if (rc < 0) {
        /* ONLY VSOUND ANSWERS THIS, so a failure means the cached
         * node is not vsound any more - the identity check, free,
         * on the call we were making anyway. The next call sweeps. */
        vsound_device_lost();
        return -1;
    }
    return 0;
}

unsigned long
vlhe_generation(void)
{
    struct vsound_chanlist cl;

    /* 0 WHEN THE MODULE IS ABSENT, which the header specifies - the
     * caller compares against its last value, and 0 never matches a
     * real generation because vsound starts at 1. */
    if (chanlist(&cl) < 0)
        return 0;
    return (unsigned long) cl.generation;
}

int
vlhe_channels(struct vlhe_channel *out, int max)
{
    struct vsound_chanlist cl;
    int i, n = 0;

    if (chanlist(&cl) < 0)
        return 0;               /* no module: zero channels, not an error */

    for (i = 0; i < (int) cl.nchan && n < max; i++) {
        struct vsound_chaninfo *ci = &cl.chan[i];

        memset(&out[n], 0, sizeof out[n]);
        out[n].index = (int) ci->index;
        out[n].pid   = (int) ci->pid;
        out[n].midi  = (ci->flags & VSOUND_CI_MIDI) ? 1 : 0;

        /* THE VOLUME SCALE IS THE DRIVER'S, NOT OURS. vsound counts to
         * VSOUND_VOL_MAX; the GUI draws 0-100 (to VLHE_VOL_BOOST with
         * Boost, 100 still being unity). Converted here so no
         * widget ever learns the driver's range. */
        out[n].volume = (int) ((ci->vol * 100 + VSOUND_VOL_MAX / 2)
                               / VSOUND_VOL_MAX);
        if (out[n].volume > VLHE_VOL_BOOST)
            out[n].volume = VLHE_VOL_BOOST;

        /* MUTE IS A DRIVER CONCEPT AS OF 2026-09-18. The channel
         * carries VSOUND_CHN_MUTED beside its volume, so `volume'
         * above is the level the user set whether or not it is
         * currently heard - a muted channel reports 60, not 0. That is
         * what lets a slider sit where it belongs with the mute lit,
         * and why nothing here has to remember a shadowed level. */
        out[n].muted = (ci->flags & VSOUND_CI_MUTED) ? 1 : 0;

        /*
         * ACTIVE MEANS "PRODUCED SOUND SINCE THE LAST POLL", not
         * "VSOUND_CI_RUNNING".
         *
         * That flag is set when a write blocks for room or acquires a
         * chunk, so a client whose writes always fit never sets it -
         * and the page drew a synth that was audibly playing as
         * "idle" (the user, on 86Box, 2026-09-18: sndserv "shows idle
         * in vmidi even if it is producing sound").
         *
         * So compare the channel's contributed-bytes counter against
         * what it read last time. Moved means producing.
         *
         * KEYED ON pid AS WELL AS SLOT, because slots are reused: a
         * new client in an old slot must not inherit the previous
         * one's byte count and look busy, or look idle because its
         * first poll happens to match.
         */
        {
            int slot = (int) ci->index;

            if (slot >= 0 && slot < VLHE_MAX_CHAN) {
                if (last_pid[slot] != (int) ci->pid) {
                    /* NEW OCCUPANT - no history, so fall back to the
                     * flag for this one poll rather than claiming
                     * idle. RUNNING is a false NEGATIVE, never a
                     * false positive, so trusting it here is safe. */
                    out[n].active = (ci->flags & VSOUND_CI_RUNNING) ? 1 : 0;
                } else {
                    out[n].active = (ci->mixed != last_mixed[slot]) ? 1 : 0;
                }
                last_pid[slot]   = (int) ci->pid;
                last_mixed[slot] = ci->mixed;
            } else {
                out[n].active = (ci->flags & VSOUND_CI_RUNNING) ? 1 : 0;
            }
        }

        if (ci->pid != 0 && ci->comm[0] != '\0') {
            strncpy(out[n].name, ci->comm, sizeof out[n].name - 1);
            out[n].name[sizeof out[n].name - 1] = '\0';
        } else if (out[n].midi) {
            strcpy(out[n].name, "MIDI Synth");
        } else {
            out[n].name[0] = '\0';
        }
        n++;
    }
    return n;
}

int
vlhe_set_volume(int index, int pid, int volume)
{
    struct vsound_vol v;
    int fd, rc;

    if (volume < 0)   volume = 0;
    if (volume > VLHE_VOL_BOOST) volume = VLHE_VOL_BOOST;

    fd = open(vsound_device(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    memset(&v, 0, sizeof v);
    v.index = (unsigned int) index;
    /* THE PID IS THE POINT, not decoration. Indices are reused: a
     * client exits, another opens, and a GUI holding "index 1 = doom"
     * from a poll 250 ms ago would set QUAKE's volume. The kernel
     * verifies and sets under one cli(), so passing it back CLOSES the
     * window rather than narrowing it (vsound.h, VSOUND_IOC_VOL). */
    v.pid = (unsigned int) pid;
    v.vol = (unsigned int) ((volume * VSOUND_VOL_MAX + 50) / 100);

    rc = ioctl(fd, VSOUND_IOC_VOL, &v);
    close(fd);
    chanlist_forget();          /* the next poll must see the new level */

    /* -ESRCH means the slot changed hands - a refresh, not an error to
     * show. The caller reports failure; the next poll corrects it. */
    return rc < 0 ? -1 : 0;
}

int
vlhe_set_mute(int index, int pid, int muted)
{
    struct vsound_mute m;
    int fd, rc;

    fd = open(vsound_device(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    memset(&m, 0, sizeof m);
    m.index = (unsigned int) index;
    /* THE PID, FOR THE REASON IN vlhe_set_volume ABOVE - the slot may
     * have changed hands since the poll this index came from, and the
     * kernel verifies under one cli(). */
    m.pid   = (unsigned int) pid;
    m.muted = muted ? 1u : 0u;

    rc = ioctl(fd, VSOUND_IOC_MUTE, &m);
    close(fd);
    chanlist_forget();

    /* -ESRCH means the slot changed hands - a refresh, not an error to
     * show. The caller reports failure; the next poll corrects it. */
    return rc < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Drives - READ ONLY until the daemon channel exists                 */
/* ------------------------------------------------------------------ */

static int g_drive_count_cache = -1;

/* FORGET THE COUNT - design/47 B5. The cache cleared itself only on a
 * call made while vdisc was unloaded, so an unload and reload inside
 * one synchronous plan (a CD-scoped Restart with a changed Drives=)
 * never saw the gap and kept the old count. The plan runner calls
 * this after every module step. */
void
vlhe_drive_count_forget(void)
{
    g_drive_count_cache = -1;
    vlhe_drives_forget();       /* a module step changes the drives too */
}

int
vlhe_drive_count(void)
{
    /*
     * HOW MANY DRIVES THE MODULE ADVERTISES. 2.2 has no /sys, so a
     * loaded module's parameters cannot be read back - which is why
     * this counts NODES THAT ANSWER rather than asking vdisc.
     *
     * A minor beyond the advertised count exists in the size arrays
     * and reads as a zero-sized device (vdisc_mod.c:2711), so an open
     * succeeding is not proof on its own; the CD-ROM layer refuses a
     * drive that was never registered, which is what distinguishes
     * them.
     */
    int i, n = 0;
#define cached g_drive_count_cache      /* B5: shared with _forget() */

    if (!vlhe_status_module_loaded("vdisc")) {
        /*
         * AND THE CACHE IS CLEARED HERE, WHICH IT WAS NOT. The
         * comment below claimed this early return did it; it returned
         * 0 and left `cached' alone, so nothing ever reset it.
         */
        cached = -1;
        return 0;
    }

    /*
     * PROBED ONCE, THEN REMEMBERED - AND THE POLLING WAS A REAL BUG,
     * not merely wasteful.
     *
     * This runs on the GUI's 250 ms refresh, and it OPENS AND CLOSES
     * every drive node to count them. Two things happen on each close
     * that have nothing to do with counting:
     *
     *   1. cdrom_release() calls lock_door(cdi, 0) unconditionally
     *      once use_count hits zero (cdrom.c:660). Since the driver
     *      now keeps the real per-drive lock (vdisc_mod.c), that call
     *      CLEARS A LOCK THE USER SET - so a locked drive unlocked
     *      itself about four times a second and eject stopped being
     *      refused. Measured on 86Box 2026-09-19 as a burst of
     *      lock_door(0) every ~101 jiffies with nobody touching the
     *      GUI.
     *   2. cdrom_open() may call tray_move(cdi, 0) to auto-close the
     *      tray (cdrom.c:500), so polling can also reverse an eject.
     *
     * THE COUNT CANNOT CHANGE WHILE THE MODULE IS LOADED. vdisc_ndevs
     * is a module parameter fixed at insmod, so one probe is as good
     * as a thousand.
     *
     * A ZERO IS NEVER CACHED, AND THAT WAS A REAL BUG - found by the
     * user on target 2026-09-23, with vdisc plainly in `lsmod' and
     * the CD page saying "The vdisc module is not loaded. No virtual
     * drives are available."
     *
     * WHAT HAPPENED: the GUI polls this on a timer. A probe that runs
     * in the window between `insmod vdisc.o' and the nodes being made
     * - which the same Load does, a step later - finds nothing,
     * caches 0, and the page believes it for the rest of the session.
     * Pressing Load again cannot help: the module is loaded, so the
     * check above passes, and the cache is returned before anything
     * looks at /dev again.
     *
     * AND THE OLD COMMENT ASSERTED THE RESET THAT DID NOT EXIST -
     * "`cached' is reset when the module goes away, by the check
     * above returning 0 before we ever get here". That return left
     * `cached' untouched. It does clear it now.
     *
     * SO: cache a POSITIVE answer, which cannot change while the
     * module is loaded, and re-probe a zero every time. A zero costs
     * one failed open of /dev/vdisc0 - the loop breaks on the first
     * error - which is cheap enough to pay on a poll, and it is the
     * only answer that can become wrong.
     */
    if (cached > 0)
        return cached;

    for (i = 0; i < VLHE_MAX_DRIVE; i++) {
        char node[VLHE_PATH_MAX];
        int fd;

        sprintf(node, "/dev/vdisc%d", i);
        fd = vdisc_node_open(node);
        if (fd < 0) {
            /*
             * -ENOMEDIUM IS AN EMPTY DRIVE, NOT AN ABSENT ONE, and
             * conflating the two made the CD page unusable: eject a
             * disc and it said "The vdisc module is not loaded. No
             * virtual drives are available", with no way to attach
             * anything again (the user, 86Box 2026-09-19).
             *
             * vdisc_open() returns -ENOMEDIUM when the minor exists
             * and has no image (vdisc_mod.c:704) and -ENXIO when it
             * was never registered. So the ERRNO is what separates
             * "a drive with no disc in it" from "no such drive" -
             * the open failing is not enough on its own.
             *
             * ENXIO/ENODEV: not registered. ENOENT: no node. Either
             * way the drives are contiguous from 0, so stop.
             */
            if (errno == ENOMEDIUM) {
                n++;
                continue;
            }
            break;
        }
        close(fd);
        n++;
    }

    /* ONLY A POSITIVE ANSWER IS KEPT - see above. */
    if (n > 0)
        cached = n;
    return n;
}
#undef cached

/*
 * ONE DRIVE'S TOKEN OF THE DAEMON'S `status' REPLY, after its `N='.
 * Two shapes: `T/A:path' since C5 (2026-10-01), when the track counts
 * ride on the status reply, and `path' alone from an older daemon -
 * a path does not start with digits-slash-digits-colon. The path runs
 * to the next ` N=' token or the end. Fills `image', and
 * `tracks'/`audio_tracks' when they were there; returns 1 if they
 * were. Public for the host tests.
 */
int
vlhe_status_token_parse(const char *p, struct vlhe_drive *d)
{
    const char *end;
    int total, audio, used = 0, had = 0;
    size_t n;

    if (sscanf(p, "%d/%d:%n", &total, &audio, &used) == 2
        && used > 0 && p[used - 1] == ':') {
        d->tracks = total;
        d->audio_tracks = audio;
        had = 1;
        p += used;
    }
    for (end = p; *end != '\0'; end++)
        if (end[0] == ' ' && end[1] >= '0' && end[1] <= '9'
            && (end[2] == '=' || (end[2] >= '0' && end[2] <= '9'
                                  && end[3] == '=')))
            break;
    n = (size_t) (end - p);
    if (n >= sizeof d->image)
        n = sizeof d->image - 1;
    memcpy(d->image, p, n);
    d->image[n] = '\0';
    return had;
}

/* THE IMAGE CACHE - C5. image_key[] is each drive's short path from
 * the last status reply; image_cache[] the full descriptor path
 * `image' answered, and image_cache_key[] the key it answered for. */
static char image_key[VLHE_MAX_DRIVE][64];
static char image_cache[VLHE_MAX_DRIVE][VLHE_PATH_MAX];
static char image_cache_key[VLHE_MAX_DRIVE][64];

/*
 * THE LAST ANSWER, KEPT UNDER A SECOND - 2026-10-03, the user: "do a
 * cache to cut down on as many requests. yes to keeping polling".
 *
 * Two pages poll this once a second each - the CD page while it is on
 * screen, the CD+G viewer always - and the viewer asks again on a disc
 * change. Each call was a `status' exchange (a SIGUSR1 to vdiscd, a
 * waker wake-up when the GUI is not root), a `tracks' one from an
 * older daemon, and (until the Lock box went, design/54 D64) an open
 * and close of every /dev/vdiscN. Kept DRIVES_KEEP_MS, every caller inside that window
 * shares one: two 1 s pollers then cost one exchange a second between
 * them, and each still sees a reply under a second old.
 *
 * STALE ONLY FOR CHANGES MADE ELSEWHERE - vlhe from a terminal, the
 * boot script - which show within two polls, as before. Every change
 * made from here forgets it, so the page that made it sees it at
 * once: any vdiscd request that is not a question (vdisc_request()),
 * a lock, a module step, and the CD+G drive radio and both viewer
 * rules (vlhe_set_cdg_*), which change which disc the viewer sees.
 */
#define DRIVES_KEEP_MS  900

static struct vlhe_drive drives_cache[VLHE_MAX_DRIVE];
static int               drives_cache_n = -1;   /* -1: nothing kept */
static int               drives_cache_count;
static struct timeval    drives_cache_when;

void
vlhe_drives_forget(void)
{
    drives_cache_n = -1;
}

static int drives_read(struct vlhe_drive *out, int max);

int
vlhe_drives(struct vlhe_drive *out, int max)
{
    int count = vlhe_drive_count();
    int n;

    if (out == NULL || max <= 0)
        return 0;
    if (drives_cache_n >= 0 && drives_cache_count == count) {
        struct timeval now;
        long ms;

        gettimeofday(&now, NULL);
        ms = (now.tv_sec - drives_cache_when.tv_sec) * 1000L
             + (now.tv_usec - drives_cache_when.tv_usec) / 1000L;
        /* ms < 0: the clock was set back - ask afresh. */
        if (ms >= 0 && ms < DRIVES_KEEP_MS) {
            n = drives_cache_n < max ? drives_cache_n : max;
            memcpy(out, drives_cache, (size_t) n * sizeof out[0]);
            return n;
        }
    }

    /* ALWAYS READ IN FULL, whatever `max' the caller passed, so a
     * small caller cannot leave a short answer for a larger one. */
    n = drives_read(drives_cache, VLHE_MAX_DRIVE);
    drives_cache_n = n;
    drives_cache_count = count;
    gettimeofday(&drives_cache_when, NULL);
    if (n > max)
        n = max;
    memcpy(out, drives_cache, (size_t) n * sizeof out[0]);
    return n;
}

static int
drives_read(struct vlhe_drive *out, int max)
{
    int count = vlhe_drive_count();
    int i, n = 0;
    int from_daemon = 0;
    int have_tracks = 0;        /* the status reply carried them - C5 */
    char reply[VDISCD_CTL_LINE];
    struct vlhe_conf st;

    /*
     * ASK THE DAEMON FIRST - it is the only thing that KNOWS.
     *
     * THE BUG THIS FIXES, found on 86Box 2026-09-19: a disc attached
     * from the command line showed as "(no disc)" in the GUI, because
     * this read /var/lib/vlhe/drives and NOTHING EVER WRITES THAT
     * FILE. design/33 section 3f says the daemon is its writer and
     * that half was never built, so the GUI was reading a file that
     * is empty on every machine.
     *
     * `status' answers from the daemon's own drive table, so it is
     * right by construction rather than by two things agreeing.
     *
     * A SHORT TIMEOUT, because this runs on the GUI's 250 ms poll
     * and a wedged daemon must not freeze the window - the drive
     * rows simply stay as they were.
     */
    st.n = 0;
    if (vdisc_request("status", reply, sizeof reply, 400) == 0
        && strncmp(reply, "ok ", 3) == 0) {
        from_daemon = 1;
    } else {
        /* NO DAEMON, OR IT DID NOT ANSWER. The state file is the
         * fallback so that `DrivesAutoLoad' can still show what WILL
         * be attached at boot - once something writes it. */
        if (vlhe_conf_read(&st, vlhe_conf_drives_path()) < 0)
            st.n = 0;
    }

    for (i = 0; i < count && n < max; i++) {
        char key[32];
        const char *img;

        memset(&out[n], 0, sizeof out[n]);
        out[n].index = i;
        sprintf(out[n].node, "/dev/vdisc%d", i);

        if (from_daemon) {
            /* THE REPLY IS `ok 0=/path 1=/other'. Find " <i>=" and
             * take to the NEXT FIELD, not the next space - design/47
             * C7, 2026-10-01. A path with a space in it used to be cut
             * at the space, acknowledged here as a display limit. The
             * field after a path can only be " <digit>=", so that is
             * the terminator; a space inside a path is kept. (The
             * daemon still sends at most 28 characters of the path;
             * the `image' verb carries the whole of it.) */
            char want[16];
            char *p;

            sprintf(want, " %d=", i);
            p = strstr(reply, want);
            /* THE AUTOLOAD FLAG - `ok a=10000000 ...', one digit per
             * drive, from a daemon that keeps the drive state
             * (2026-10-03). An older daemon has no `a=' and every box
             * reads clear, which is what it always showed. */
            {
                const char *af = (strncmp(reply, "ok a=", 5) == 0)
                                 ? reply + 5 : NULL;
                int k;

                for (k = 0; af != NULL && k < i; k++)
                    if (af[k] != '0' && af[k] != '1')
                        af = NULL;
                out[n].autoload = (af != NULL && af[i] == '1');
            }
            /* AND /dev/cdrom's DRIVE - ` c=N' (2026-10-03). */
            {
                const char *cf = strstr(reply, " c=");
                const char *first = strstr(reply, " 0=");

                out[n].cdrom = (cf != NULL && (first == NULL || cf < first)
                                && atoi(cf + 3) == i);
            }
            if (p != NULL) {
                if (vlhe_status_token_parse(p + strlen(want), &out[n]))
                    have_tracks = 1;
                out[n].attached = 1;
                if (i < VLHE_MAX_DRIVE) {
                    strncpy(image_key[i], out[n].image, sizeof image_key[i] - 1);
                    image_key[i][sizeof image_key[i] - 1] = '\0';
                }
            }
            if (i < VLHE_MAX_DRIVE && !out[n].attached)
                image_key[i][0] = '\0';   /* re-ask on the next attach */
            n++;
            continue;
        }

        sprintf(key, "Drive%d", i);
        img = vlhe_conf_get(&st, "Drives", key, "");
        if (img[0] != '\0') {
            strncpy(out[n].image, img, sizeof out[n].image - 1);
            out[n].image[sizeof out[n].image - 1] = '\0';
            out[n].attached = 1;
        }
        sprintf(key, "Autoload%d", i);
        out[n].autoload = vlhe_conf_get_int(&st, "Drives", key, 0) != 0;
        out[n].cdrom = vlhe_conf_get_int(&st, "Drives", "Cdrom", 0) == i;

        /*
         * WHAT IS NOT FILLED IN, AND WHY IT IS NOT GUESSED.
         *
         * tracks, audio_tracks, sessions and kind all come from the
         * TOC, which only the daemon holds - it parsed the image. The
         * CD-ROM layer would answer CDROMREADTOCHDR, but doing that
         * from here means opening a drive a client may be using and
         * issuing ioctls behind the daemon's back.
         *
         * `mounted' likewise wants /proc/mounts parsed against the
         * node, which is cheap but is a claim about a device we are
         * not otherwise touching.
         *
         * LEFT ZERO DELIBERATELY. The GUI draws what it is given; a
         * fabricated track count would be worse than an empty column.
         * The daemon channel (design/33 section 3f) is where these
         * come from.
         */
        n++;
    }
    /*
     * AND THE TRACK COUNTS, in a second exchange.
     *
     * A SEPARATE VERB because `status' cannot carry them: its reply
     * is parsed by reading to the next space, so a path containing
     * one already truncates and appending fields would make that
     * worse. See cmd_tracks() in vdiscd.c.
     *
     * ONLY WHEN THE DAEMON ANSWERED STATUS. If it is not there, the
     * drive rows came from the state file and there is nothing to ask
     * - and a second timeout per refresh on a machine with no daemon
     * would cost 400 ms of the GUI's 250 ms poll.
     *
     * A FAILURE LEAVES THE COUNTS AT ZERO, which is what they were
     * before this existed. The page says "0 tracks", exactly as it
     * did, rather than refusing to draw.
     */
    /* `tracks' ONLY WHEN `status' DID NOT CARRY THEM - an older daemon.
     * One request a tick instead of two (C5). */
    if (from_daemon && !have_tracks
        && vdisc_request("tracks", reply, sizeof reply, 400) == 0
        && strncmp(reply, "ok ", 3) == 0) {
        int k;

        for (k = 0; k < n; k++) {
            char want[16];
            char *p;

            sprintf(want, " %d=", out[k].index);
            p = strstr(reply, want);
            if (p != NULL) {
                int total = 0, audio = 0;

                if (sscanf(p + strlen(want), "%d/%d", &total, &audio)
                    == 2) {
                    out[k].tracks = total;
                    out[k].audio_tracks = audio;
                }
            }
        }
    }

    return n;
}

int
vlhe_drive_image(int index, char *out, int max)
{
    char reply[VDISCD_CTL_LINE];
    char cmd[32];

    if (out == NULL || max <= 0 || index < 0)
        return -1;
    out[0] = '\0';

    /*
     * ASKED ONCE PER DISC, NOT ONCE PER TICK - design/47 C5. The CD
     * page shows this for every attached drive every second, and the
     * answer changes only when the drive's image does - which the
     * status reply's short path (vlhe_drives()) says. The last answer
     * is kept per drive with that key; a different key, or an empty
     * one, asks again.
     */
    if (index < VLHE_MAX_DRIVE && image_key[index][0] != '\0'
        && image_cache[index][0] != '\0'
        && strcmp(image_key[index], image_cache_key[index]) == 0) {
        strncpy(out, image_cache[index], (size_t) max - 1);
        out[max - 1] = '\0';
        return 0;
    }

    sprintf(cmd, "image %d", index);
    if (vdisc_request(cmd, reply, sizeof reply, 400) != 0)
        return -1;
    if (strncmp(reply, "ok ", 3) != 0)
        return -1;                      /* err no such drive / no image */

    /*
     * EVERYTHING AFTER "ok ", TO END OF LINE - so a path with
     * SPACES survives, which is the other thing `status' cannot do.
     * /mnt/My Discs/foo.ccd is an ordinary name.
     */
    strncpy(out, reply + 3, (size_t) max - 1);
    out[max - 1] = '\0';
    if (index < VLHE_MAX_DRIVE && out[0] != '\0') {
        strncpy(image_cache[index], out, sizeof image_cache[index] - 1);
        image_cache[index][sizeof image_cache[index] - 1] = '\0';
        strncpy(image_cache_key[index], image_key[index],
                sizeof image_cache_key[index] - 1);
        image_cache_key[index][sizeof image_cache_key[index] - 1] = '\0';
    }
    return out[0] != '\0' ? 0 : -1;
}

/*
 * RECORD AN IMAGE THAT LOADED. Move-to-front, deduplicated, capped
 * at VLHE_MAX_RECENT.
 *
 * THIS DID NOT EXIST UNTIL 2026-09-19 AND THE MENU WAS ALWAYS EMPTY.
 * vlhe_recent() read `Recent Images' and vlhe_recent_remove() rewrote
 * it, so the read side and the forget side were both complete - and
 * nothing in the real backend had ever WRITTEN a path. The fake
 * backend does it in its own attach and ships five entries
 * pre-populated (vlhe_backend_fake.c:371), which is why the menu
 * looked right in GUI development and was `(no recent images)' on
 * every real machine.
 *
 * WRITES IMMEDIATELY, like vlhe_recent_remove() and for its reason:
 * an attach is an action the user took, not a form field behind
 * Apply, and there is no later commit point that would carry it.
 */
static void
recent_touch(const char *path)
{
    char keep[VLHE_MAX_RECENT][VLHE_PATH_MAX];
    int  n = 0, i;
    const char *up;

    if (path == NULL || path[0] == '\0')
        return;

    load_config();

    /* The new path goes first; the rest follow in order, minus any
     * copy of it. Reading the old list before writing any of it back
     * means a repeat load reorders rather than duplicating. */
    strncpy(keep[n], path, VLHE_PATH_MAX - 1);
    keep[n][VLHE_PATH_MAX - 1] = '\0';
    n++;

    for (i = 0; i < VLHE_MAX_RECENT && n < VLHE_MAX_RECENT; i++) {
        char key[32];
        const char *p;

        sprintf(key, "Recent%d", i);
        p = vlhe_conf_get(&cfg_usr, "Recent Images", key, "");
        if (p[0] == '\0' || strcmp(p, path) == 0)
            continue;
        strncpy(keep[n], p, VLHE_PATH_MAX - 1);
        keep[n][VLHE_PATH_MAX - 1] = '\0';
        n++;
    }

    for (i = 0; i < VLHE_MAX_RECENT; i++) {
        char key[32];

        sprintf(key, "Recent%d", i);
        if (i < n)
            vlhe_conf_set(&cfg_usr, "Recent Images", key, keep[i]);
        else
            vlhe_conf_unset(&cfg_usr, "Recent Images", key);
    }

    /*
     * UPDATE A FILE THAT EXISTS; NEVER CREATE ONE - 2026-09-27.
     *
     * THIS WROTE UNCONDITIONALLY AND IT WAS A REGRESSION. Attaching
     * a disc in PORTABLE mode generated the whole 162-line user
     * template from scratch, to hold one `Recent0=' line, with no
     * Save and nothing said. The user found it: *"something created
     * a user-root.conf file in portable mode"*, and their account of
     * how it used to behave is the specification - *"recent images
     * only appeared in portable mode during that session. if I
     * closed it without saving settings the recent images was gone
     * but they were there if I loaded and unloaded and kept the
     * program open"*. In memory for the session; on disk only if
     * there is already a file.
     *
     * THE RULE CAME LATER THAN THIS CODE, WHICH IS WHY IT WAS
     * MISSED. `recent_touch()' got its writer in 97e4ba4
     * (2026-09-20); `Apply is memory-only for the user file' landed
     * in 828acbb (2026-09-26, row 88) and only taught
     * vlhe_commit(). These two writers predate it by six days and
     * bypass it completely by calling the template writer direct.
     *
     * AND IT IS THE CONSENT RULE, not a storage preference: a
     * portable tree promises that deleting the folder removes every
     * trace, and a file nobody asked for is the one thing that
     * promise cannot survive.
     *
     * THE MEMORY COPY IS STILL UPDATED ABOVE, so the list works for
     * the whole session either way - which is exactly the behaviour
     * being restored.
     */
    up = cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
    if (up != NULL && file_exists(up)
        && vlhe_conf_template_write(&cfg_usr, up, VLHE_TPL_USER, 0) == 0)
        /* NOT DIRTY ONCE IT IS ON DISK - see vlhe_set_prefs(). The
         * conf_set() calls above raised the flag and this is the
         * commit point, so nothing else would ever lower it. The
         * result is now TESTED rather than discarded, which is what
         * makes clearing it honest. */
        cfg_usr.dirty = 0;
}

int
vlhe_attach_reply(const char *reply)
{
    if (reply == NULL)
        return VLHE_ATTACH_NO_ANSWER;
    if (strncmp(reply, "ok", 2) == 0)
        return 0;
    if (strncmp(reply, "err denied", 10) == 0)
        return VLHE_ATTACH_DENIED;
    if (strncmp(reply, "err missing", 11) == 0)
        return VLHE_ATTACH_MISSING;
    return VLHE_ATTACH_REFUSED;
}

int
vlhe_attach(int index, const char *path)
{
    /*
     * THROUGH THE DAEMON - design/33 section 3f. /dev/vdiscctl admits
     * one opener and that is vdiscd; attaching also needs the image
     * parsed, which is the daemon's job and its ~1500 lines. So the
     * GUI asks rather than doing it itself.
     *
     * A SHORT TIMEOUT IS WRONG HERE. Parsing a .cue with many tracks
     * off a 3.5 MB/s PATA disk is not instant, and a GUI that gives
     * up on a working daemon is worse than one that pauses. Five
     * seconds; a wedged daemon is reported as such (section 3g) and
     * the user is offered a restart.
     */
    char req[VDISCD_CTL_LINE];
    char reply[VDISCD_CTL_LINE];
    int  r;

    if (path == NULL || index < 0)
        return VLHE_ATTACH_REFUSED;

    sprintf(req, "attach %d %.280s", index, path);

    if (vdisc_request(req, reply, sizeof reply, 5000) != 0)
        return VLHE_ATTACH_NO_ANSWER;

    /* THE DAEMON'S OWN WORDS DECIDE - and since 2026-10-05 they say
     * why, which vlhe_attach_reply() turns into a reason the CD page
     * can name. Anything starting "ok" worked. */
    r = vlhe_attach_reply(reply);
    if (r != 0)
        return r;

    /* ONLY A LOAD THAT HAPPENED IS REMEMBERED, which is why this sits
     * behind the reply check rather than beside the send. A refused
     * swap over a busy drive (vdiscd's -EBUSY path) records nothing,
     * and neither does an image the daemon could not parse. */
    recent_touch(path);
    return 0;
}

int
vlhe_detach(int index)
{
    char req[VDISCD_CTL_LINE];
    char reply[VDISCD_CTL_LINE];

    if (index < 0)
        return -1;

    /*
     * NO LOCK CHECK - the Lock box is gone (design/54 D64, 2026-10-05).
     * What it was for still holds without it: this detach is NOT
     * forced, and the kernel refuses it while anything has the drive
     * open (vdisc_do_detach(), `use_count > 0') - so a mounted or
     * playing disc cannot be pulled out from here.
     */
    sprintf(req, "detach %d", index);

    /* SHORTER THAN ATTACH because there is no image to parse - the
     * daemon closes a file and tells the kernel. */
    if (vdisc_request(req, reply, sizeof reply, 2000) != 0)
        return -1;

    return (strncmp(reply, "ok", 2) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* The CD transport - design/34 6e2                                   */
/* ------------------------------------------------------------------ */

/*
 * PLAY, STOP, PAUSE, RESUME - BY IOCTL ON THE DRIVE NODE, the way
 * KsCD and grip do it. design/45.
 *
 * THIS USED TO GO THROUGH THE DAEMON'S CONTROL CHANNEL AND THAT WAS
 * THE BUG. `f749a21' gave vdiscd the four verbs and `5ddf621' wired
 * these functions to them, 118 seconds apart on 2026-09-26 - and
 * `cmd_play()' starts the audio child without telling the MODULE
 * anything. So the daemon played while `dev->audio_status' stayed
 * `CDROM_AUDIO_NO_STATUS', `CDROMSUBCHNL' reported the drive
 * stopped, and every reader of that ioctl believed it.
 *
 * WHAT IT COST: the CD+G page correctly declined to decode a disc it
 * was told was not playing, so the picture stayed black and the
 * clock read `--:--' while audio came out. Two target sessions to
 * find, because the page said nothing when it skipped the decode.
 * `tests/logs/2026-09-28-cdg-no-decode-traced/' has the line that
 * settled it: `no decode: lba 0, playing 0'.
 *
 * AND THE MODULE DISCARDS THE CHILD'S REPORTS ON PURPOSE.
 * `vdisc_do_put_pos()' will not let a position report move the
 * status off NO_STATUS - "only PLAYMSF and PLAYTRKIND" - because a
 * late in-flight report used to resurrect a stopped drive (86Box run
 * 77, where an ERROR landed after a STOP and locked KsCD out for the
 * session). That guard is right and this change leaves it alone.
 *
 * SO THE FIX IS TO STOP HAVING TWO WAYS IN. The ioctl path already
 * does both halves in one place - it tells the daemon to play AND
 * sets the state (`vdisc_mod.c:1200') - which is why grip works.
 *
 * THE GUI ALREADY DOES EVERYTHING ELSE THE PERIOD WAY: it reads the
 * TOC once at attach, decodes CD+G from its own read-only copy of
 * the image, and takes position from `CDROMSUBCHNL' (see
 * `vlhe_cd_position()' below - "It asks no daemon"). Play was the
 * one verb that did not, and that asymmetry was the whole fault.
 *
 * THE DAEMON'S VERBS STAY. They are a reasonable interface for
 * something that is not a CD player; they simply have no caller
 * here now.
 */
static int
transport_ioctl(int index, int req, void *arg)
{
    char node[VLHE_PATH_MAX];
    int  fd, rc;

    if (index < 0 || index >= VLHE_MAX_DRIVE)
        return -1;
    sprintf(node, "/dev/vdisc%d", index);

    /* O_NONBLOCK for the same reason vlhe_cd_position() uses it: an
     * audio disc with no data track can block on open while the
     * layer looks for a filesystem, and these are GUI buttons. */
    fd = vdisc_node_open(node);
    if (fd < 0)
        return -1;              /* open's own errno, untouched */
    rc = ioctl(fd, req, arg);
    if (rc != 0) {
        /* SAVED ACROSS THE CLOSE. `close()' can set errno itself, and
         * on a failing ioctl that would replace the answer with a
         * footnote. */
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * `secs' IS AN OFFSET INTO THE TRACK, which is how seeking is done -
 * `vlhe_backend.h' defines it that way and there is deliberately no
 * `vlhe_cd_seek()'. The caller holds the TOC, so it does the
 * arithmetic; here that becomes the START address.
 *
 * AND PLAY RUNS TO THE LEAD-OUT, not to the end of the track,
 * because that is what a CD player does - press play on track 3 and
 * it continues into 4. The module clips the range at the first DATA
 * track on the way (`design/26' B6), which is a guard the control
 * channel never had: `cmd_play()' checks only the starting track, so
 * a range crossing into data played Mode-1 sectors as full-level
 * PCM.
 */
int vlhe_cd_play(int index, int track, int secs)
{
    /* 99 IS THE CD MAXIMUM, and there is no shared constant for it -
     * `vlhe_mod_cdg.c' spells the same number as CDG_MAX_TOC. A
     * header would be better; naming it twice is not worth a change
     * to the public interface while this is the only other caller. */
    struct vlhe_cd_track toc[99];
    struct cdrom_msf     msf;
    int start, end, n;

    /*
     * EVERY FAILURE SETS `errno', SO THE CALLER CAN SAY WHICH -
     * 2026-09-28, after this returned a bare -1 and the GUI could
     * only say "Could not start playback".
     *
     * THAT IS THE SAME DEFECT I HAD JUST TRACED IN `on_poll()' and
     * wrote again an hour later: a failure path that reports nothing
     * costs a target run to narrow. There is no trace facility in
     * this file and adding one for three lines would be worse, so
     * the reason travels as an errno the caller already knows how to
     * print.
     */
    n = vlhe_cd_toc(index, toc, 99);
    if (n <= 0) {
        /* The TOC comes over the daemon's control channel, so this is
         * "no daemon", "no disc" or a 2000 ms timeout - not anything
         * about the track asked for. */
        errno = ENOMEDIUM;
        return -1;
    }
    if (track < 1 || track > n) {
        errno = EINVAL;
        return -1;
    }

    start = toc[track - 1].start + secs * 75;
    /* THE LEAD-OUT IS THE LAST TRACK'S END, which the TOC gives as
     * start+length rather than as an entry of its own. */
    end   = toc[n - 1].start + toc[n - 1].length;
    if (start < 0 || start >= end) {
        /* A seek past the end of the disc, which is the caller's
         * arithmetic rather than a device problem. */
        errno = ERANGE;
        return -1;
    }

    /* THE +150 IS THE WIRE FORMAT. vdisc_msf_to_lba() subtracts it
     * again on the way in - `msf.h' has why the driver adds it and
     * the layer strips it. Getting this backwards shifts every
     * address by two seconds and looks plausible until diffed. */
    start += CD_MSF_OFFSET;
    end   += CD_MSF_OFFSET;

    msf.cdmsf_min0   = start / (75 * 60);
    msf.cdmsf_sec0   = (start / 75) % 60;
    msf.cdmsf_frame0 = start % 75;
    msf.cdmsf_min1   = end / (75 * 60);
    msf.cdmsf_sec1   = (end / 75) % 60;
    msf.cdmsf_frame1 = end % 75;

    return transport_ioctl(index, CDROMPLAYMSF, &msf);
}

int vlhe_cd_stop(int index)
{ return transport_ioctl(index, CDROMSTOP, (void *) 0); }
int vlhe_cd_pause(int index)
{ return transport_ioctl(index, CDROMPAUSE, (void *) 0); }
int vlhe_cd_resume(int index)
{ return transport_ioctl(index, CDROMRESUME, (void *) 0); }

/*
 * THE TOC, SO THE CLIENT CAN DO ITS OWN ARITHMETIC - design/34 6e2.
 *
 * KsCD's arrangement, and the reason this exists rather than a
 * `seek' verb: `cdrom.c:431' computes a track-relative position as
 * `(cur_frame - trk[n].start) / 75' from a TOC it read once and
 * keeps, with CDROMSUBCHNL for the frame. It asks no daemon.
 *
 * READ ONCE AT ATTACH, not per poll. Nothing about a disc's layout
 * changes while it is in the drive, and a viewer polling four times
 * a second must not send a request each time.
 *
 * Returns the number of tracks filled, or -1.
 */
int
vlhe_cd_toc(int index, struct vlhe_cd_track *out, int max)
{
    char  req[32], reply[VDISCD_CTL_LINE];
    char *p;
    int   n = 0;

    if (index < 0 || index >= VLHE_MAX_DRIVE || out == NULL || max <= 0)
        return -1;

    sprintf(req, "toc %d", index);
    if (vdisc_request(req, reply, sizeof reply, 2000) != 0)
        return -1;
    if (strncmp(reply, "ok toc", 6) != 0)
        return -1;

    /* "ok toc <drive> <count> 1=150/20000/0 2=20150/18000/0 ..."
     *
     * SKIP THE SPACE BEFORE SKIPPING A FIELD - 2026-09-28, and this
     * was off by one field for every disc.
     *
     * `reply + 6' lands ON the space that follows "ok toc", so
     * `strchr(reply + 6, ' ')' found THAT space rather than the one
     * after the drive number. Two searches therefore advanced past
     * the drive alone, `sscanf' met the COUNT where it wanted
     * "<n>=<start>/<len>/<data>", and the loop broke on its first
     * iteration with n = 0.
     *
     * THE CALLER READS 0 AS "NO DISC". `vlhe_cd_play()' maps n <= 0
     * to ENOMEDIUM, so every transport button on the CD+G page
     * failed with "no disc, or the daemon did not answer" while the
     * daemon was answering perfectly - 10 tracks, and the same page
     * was drawing that disc's graphics from its own read of the
     * image at the time. Measured on 86Box: three presses, three
     * failures, DAEMON.LOG lines 43, 201 and 212.
     *
     * NOT A TRUNCATION, WHICH WAS THE FIRST THEORY. This disc's
     * reply is 174 bytes against a 320-byte line, so it arrived
     * whole and was parsed wrong.
     */
    p = reply + 6;
    while (*p == ' ')                    /* onto the drive number */
        p++;
    p = strchr(p, ' ');                  /* past the drive number */
    if (p == NULL)
        return -1;
    while (*p == ' ')                    /* onto the count        */
        p++;
    p = strchr(p, ' ');                  /* past the count; NULL
                                          * when the disc has no
                                          * tracks, which the loop
                                          * below reads as n = 0 */

    while (p != NULL && n < max) {
        int num, start, len, data;

        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;
        if (sscanf(p, "%d=%d/%d/%d", &num, &start, &len, &data) != 4)
            break;
        out[n].num     = num;
        out[n].start   = start;
        out[n].length  = len;
        out[n].is_data = data;
        n++;
        p = strchr(p, ' ');
    }
    return n;
}

/*
 * WHERE PLAYBACK HAS REACHED - FROM THE KERNEL, NOT THE DAEMON.
 *
 * `CDROMSUBCHNL' on the drive node, which the module answers from
 * the position its CDDA child pushes with VDISC_IOC_PUT_POS. THE
 * DAEMON CANNOT ANSWER THIS: the child reports straight to the
 * kernel rather than relaying through its parent, deliberately, so
 * that a wedged child cannot block the block server
 * (`vdiscd_play.c:45'). A `pos' verb was written and removed for
 * that reason.
 *
 * SO THIS NEEDS NO DAEMON AT ALL - any process that can open
 * /dev/vdiscN can read it, which is also what a period CD player
 * does.
 *
 * Returns the absolute LBA, or -1. `track' and `playing' are filled
 * when non-NULL.
 */
int
vlhe_cd_position(int index, int *track, int *playing)
{
    char node[VLHE_PATH_MAX];
    struct cdrom_subchnl sc;
    int fd, lba;

    if (index < 0 || index >= VLHE_MAX_DRIVE)
        return -1;

    /*
     * /proc/vdisc FIRST - design/47 G1, 2026-10-01. The CD+G viewer
     * asks this fifteen times a second, and the open-ioctl-close below
     * reached the uniform layer's cdrom_release() on every close, which
     * called the driver's lock_door(0): 15 opens and 15 kernel-log
     * lines a second. The module now publishes the same answer in
     * /proc; a module built before a1e7205 has no entry, and the
     * ioctl path stays for it.
     */
    lba = vlhe_vdisc_position(index, track, playing);
    if (lba >= 0)
        return lba;
    if (access("/proc/vdisc", R_OK) == 0)
        return -1;              /* the entry exists and says: not attached */

    sprintf(node, "/dev/vdisc%d", index);

    /* O_NONBLOCK: an audio disc with no data track can otherwise
     * block on open while the layer looks for a filesystem, and
     * this is called from a GUI poll. */
    fd = vdisc_node_open(node);
    if (fd < 0)
        return -1;

    memset(&sc, 0, sizeof sc);
    sc.cdsc_format = CDROM_LBA;
    if (ioctl(fd, CDROMSUBCHNL, &sc) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    lba = sc.cdsc_absaddr.lba;
    if (track != NULL)
        *track = sc.cdsc_trk;
    if (playing != NULL)
        *playing = (sc.cdsc_audiostatus == CDROM_AUDIO_PLAY);
    return lba;
}


/*
 * "LOAD AT STARTUP" - design/36 row 64, built 2026-10-03.
 *
 * OURS RATHER THAN THE KERNEL'S: whether this drive's image comes back
 * at the next load (at boot installed, at Load portable). It is a key
 * in the drive state file, which the daemon owns - so this ASKS vdiscd
 * over the channel, as an attach does, and the daemon records it. The
 * GUI never writes the file: as the `vlhe' account's, a user could
 * not, and two writers would disagree.
 *
 * Nothing comes back unless [CD Settings] DrivesAutoLoad = 1 as well -
 * the master switch the user kept.
 */
int
vlhe_set_cdrom_drive(int index)
{
    char req[32];
    char reply[VDISCD_CTL_LINE];

    if (index < 0 || index >= VLHE_MAX_DRIVE)
        return -1;
    sprintf(req, "cdrom %d", index);
    if (vdisc_request(req, reply, sizeof reply, 2000) != 0)
        return -1;
    return strncmp(reply, "ok", 2) == 0 ? 0 : -1;
}

int
vlhe_set_autoload(int index, int on)
{
    char req[64];
    char reply[VDISCD_CTL_LINE];

    if (index < 0 || index >= VLHE_MAX_DRIVE)
        return -1;
    sprintf(req, "autoload %d %d", index, on ? 1 : 0);
    if (vdisc_request(req, reply, sizeof reply, 2000) != 0)
        return -1;
    return strncmp(reply, "ok", 2) == 0 ? 0 : -1;
}

int
vlhe_recent(char out[][VLHE_PATH_MAX], int max)
{
    /* THE RECENT LIST IS PER-USER, and the one part of the CD module
     * that needs no daemon at all - it is a preference, not state
     * (design/33 section 1). Ten entries, most recent first. */
    int i, n = 0;

    load_config();
    for (i = 0; i < 10 && n < max; i++) {
        char key[32];
        const char *p;

        sprintf(key, "Recent%d", i);
        p = vlhe_conf_get(&cfg_usr, "Recent Images", key, "");
        if (p[0] == '\0')
            continue;
        strncpy(out[n], p, VLHE_PATH_MAX - 1);
        out[n][VLHE_PATH_MAX - 1] = '\0';
        n++;
    }
    return n;
}

int
vlhe_recent_remove(const char *path)
{
    char keep[10][VLHE_PATH_MAX];
    int n = 0, i;
    const char *up;

    load_config();

    /* Rebuild the list without it, then renumber - so there is never a
     * gap for the reader to skip over. */
    for (i = 0; i < 10; i++) {
        char key[32];
        const char *p;

        sprintf(key, "Recent%d", i);
        p = vlhe_conf_get(&cfg_usr, "Recent Images", key, "");
        if (p[0] == '\0' || strcmp(p, path) == 0)
            continue;
        strncpy(keep[n], p, VLHE_PATH_MAX - 1);
        keep[n][VLHE_PATH_MAX - 1] = '\0';
        n++;
    }

    for (i = 0; i < 10; i++) {
        char key[32];

        sprintf(key, "Recent%d", i);
        if (i < n)
            vlhe_conf_set(&cfg_usr, "Recent Images", key, keep[i]);
        else
            vlhe_conf_unset(&cfg_usr, "Recent Images", key);
    }

    up = cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
    if (up == NULL)
        return -1;

    /*
     * NO FILE, NO WRITE - AND THAT IS SUCCESS, NOT FAILURE.
     * 2026-09-27, the same fix as recent_touch() above; the
     * reasoning is there. The forget already took effect in memory
     * a few lines up, which is all a session-only list needs, so
     * returning -1 would report a failure to do something that was
     * in fact done.
     */
    if (!file_exists(up))
        return 0;

    /* WRITES IMMEDIATELY WHEN THERE IS A FILE, AND DELIBERATELY -
     * this is not a form field behind Apply. It is an action the
     * user took in a dialog that has already closed, so there is no
     * later commit point to carry it. */
    {
        int rc = vlhe_conf_template_write(&cfg_usr, up, VLHE_TPL_USER, 0);

        /* AND NOT DIRTY AFTERWARDS - see vlhe_set_prefs(). Only on
         * success: a failed write leaves memory differing from the
         * file, which is what dirty means. */
        if (rc == 0)
            cfg_usr.dirty = 0;
        return rc;
    }
}

/* ------------------------------------------------------------------ */
/* Majors and module options - the CD Settings Options tab            */
/* ------------------------------------------------------------------ */

/*
 * Is a block major registered BY SOMEBODY ELSE? READ AT THE MOMENT OF
 * ASKING, never cached - the header is explicit, and a stale answer
 * here is how a user's hardware stops working.
 *
 * "BY SOMEBODY ELSE" IS THE PART THAT WAS MISSING, found on target
 * 2026-09-23: with vdisc loaded at major 63 the Options tab said
 * "That major is already in use - pick another" about the major it
 * was using, correctly, at that moment. The check saw OUR OWN module
 * in /proc/devices and could not tell it apart from a collision.
 *
 * SO THE NAME IS COMPARED TOO. /proc/devices carries one, and ours is
 * "vdisc" - registered by vdisc_mod.c's register_blkdev(). A line
 * naming us is not a conflict; it is the configuration working.
 */
static int
major_in_use(int num)
{
    FILE *fp;
    char line[128];
    int in_block = 0, found = 0;

    /* EMPTY IS NOT SET - design/49 N4. This tested for NULL only, so
     * an empty value opened "" and reported every major free. Every
     * other reader of a VLHE_* override tests both. */
    {
        const char *v = getenv("VLHE_PROC_DEVICES");

        fp = fopen((v != NULL && *v != '\0') ? v : "/proc/devices", "r");
    }
    if (fp == NULL)
        return 0;               /* cannot tell: offer it, do not lie */

    while (fgets(line, sizeof line, fp) != NULL) {
        int n;
        char name[64];

        if (strncmp(line, "Block", 5) == 0) {
            in_block = 1;
            continue;
        }
        if (strncmp(line, "Character", 9) == 0) {
            in_block = 0;
            continue;
        }
        if (!in_block)
            continue;
        if (sscanf(line, "%d %63s", &n, name) == 2 && n == num) {
            /* OURS DOES NOT COUNT - see the header above. */
            if (strcmp(name, "vdisc") == 0)
                break;
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

int
vlhe_majors(int which, struct vlhe_major *out, int max)
{
    /* THE TWO LISTS, and the second is an opt-in for a reason the
     * header spells out: taking a real driver's major is how someone
     * else's hardware stops working. */
    static const struct { int num; const char *what; } local[] = {
        { 60, "Local/experimental" },
        { 61, "Local/experimental" },
        { 62, "Local/experimental" },
        { 63, "Local/experimental (the reference uses this)" }
    };
    static const struct { int num; const char *what; } mimic[] = {
        { 25, "Matsushita / Panasonic CR-5xx, second unit" },
        { 26, "Matsushita / Panasonic CR-5xx, third unit" },
        { 27, "Matsushita / Panasonic CR-5xx, fourth unit" },
        { 28, "Matsushita / Panasonic CR-5xx, fifth unit" },
        { 15, "Sony CDU-31A / CDU-33A" },
        { 24, "Sony CDU-535" }
    };
    int i, n = 0, count;

    count = (which == VLHE_MAJ_MIMIC)
            ? (int)(sizeof mimic / sizeof mimic[0])
            : (int)(sizeof local / sizeof local[0]);

    for (i = 0; i < count && n < max; i++) {
        int num = (which == VLHE_MAJ_MIMIC) ? mimic[i].num : local[i].num;
        const char *what = (which == VLHE_MAJ_MIMIC) ? mimic[i].what
                                                     : local[i].what;

        out[n].num = num;
        strncpy(out[n].what, what, sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = '\0';
        /* MARKED, NEVER SILENTLY DROPPED - the header's word. A menu
         * that quietly omits an entry teaches nothing; one that greys
         * it says why. */
        out[n].in_use = major_in_use(num);
        n++;
    }
    return n;
}

/*
 * WHERE WE REMEMBER WHAT /dev/dsp WAS - design/38 section 9.
 *
 * The same shape and the same reasoning as the cdrom one below: a
 * file rather than memory, because the process that made the change
 * may not be the one that undoes it. An unload after a reboot, or one
 * run by the init script rather than the GUI, must still be able to
 * put the node back.
 */
/*
 * NO LONGER CALLED - 2026-09-25, the same fold that retired
 * `vlhe_cdrom_saved_path()'. `dsp.was' held WHAT a node was and
 * never WHICH node, so the restore had to ask the config for the
 * path; the session record carries both and there is nothing left
 * for this file to do. Kept as a symbol for the reason the cdrom one
 * is - the fake backend defines a match, and removing one means
 * removing both.
 */
const char *
vlhe_dsp_saved_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *env = getenv("VLHE_DSP_SAVED");

    if (env != NULL && *env != '\0') {
        strncpy(buf, env, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        return buf;
    }

    env = getenv("VLHE_MODULE_DIR");
    if (env != NULL && *env != '\0'
        && strlen(env) + sizeof "/dsp.was" <= sizeof buf) {
        strcpy(buf, env);
        strcat(buf, "/dsp.was");
        return buf;
    }

    if (vlhe_self_is_trial()
        && vlhe_self_path("dsp.was", buf, sizeof buf))
        return buf;

    strcpy(buf, "/var/lib/vlhe/dsp.was");
    return buf;
}

/*
 * WHERE THE SAVED `/dev/cdrom' TARGET LIVES.
 *
 * A FILE RATHER THAN MEMORY, and that is the whole feature. The
 * user's complaint, 2026-09-23: "I have run into times where I forgot
 * it was set and had to swap it back to my real drive." Knowledge
 * held in the process that made the change is lost the moment that
 * process exits - so an unload after a reboot, or one run by the init
 * script rather than by the GUI, must still be able to put the link
 * back.
 *
 * THE THREE CASES ARE THE JOURNAL'S, deliberately - see
 * journal_path() in vlhe_journal.c, which reasoned all of them
 * through and had a real bug in the middle case. An override wins; a
 * portable copy keeps it beside its binary, because a copy that
 * promises to touch nothing of the system must not write to /var;
 * otherwise /var/lib/vlhe, beside `drives'.
 */
/*
 * NO LONGER CALLED BY ANYTHING - 2026-09-25. `cdrom.was' was folded
 * into the session file (design/40), so nothing reads or writes this
 * path any more. Kept for now because the fake backend defines a
 * matching symbol and removing one means removing both; deleting it
 * is a cleanup of its own, not part of the fold.
 */
const char *
vlhe_cdrom_saved_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *env = getenv("VLHE_CDROM_SAVED");

    if (env != NULL && *env != '\0') {
        strncpy(buf, env, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        return buf;
    }

    env = getenv("VLHE_MODULE_DIR");
    if (env != NULL && *env != '\0'
        && strlen(env) + sizeof "/cdrom.was" <= sizeof buf) {
        strcpy(buf, env);
        strcat(buf, "/cdrom.was");
        return buf;
    }

    if (vlhe_self_is_trial()
        && vlhe_self_path("cdrom.was", buf, sizeof buf))
        return buf;

    strcpy(buf, "/var/lib/vlhe/cdrom.was");
    return buf;
}

/*
 * WHAT `/dev/cdrom' IS, WITHOUT CHANGING IT.
 *
 * lstat() AND NOT stat(), which is the whole distinction this
 * function turns on: stat() FOLLOWS a symlink and would report the
 * block device at the far end, making a symlink indistinguishable
 * from a real node - and those two cases get opposite treatment.
 */
int
vlhe_cdrom_link(struct vlhe_cdrom_link *out)
{
    struct stat sb;
    int  n;

    if (out == NULL)
        return -1;

    memset(out, 0, sizeof *out);
    out->kind = VLHE_CDROM_ABSENT;

    /*
     * WHAT WE SAVED, if anything - read first, so a caller can report
     * an outstanding restore even when the link itself has since been
     * changed by hand.
     *
     * FROM THE SESSION FILE, NOT `cdrom.was' - 2026-09-25, the
     * user's call: "86box is the only machine carrying a cdrom.was so
     * there is no point keeping it just for that one machine."
     * design/40 section 6 had kept both during a migration; the
     * migration has no population to protect.
     *
     * THE SESSION RECORD SPELLS IT `link <target>' OR `absent', which
     * is the dsp vocabulary - one restore reads both - so the prefix
     * comes off here. `absent' leaves `saved' empty, which is what
     * every caller already reads as "nothing to put back".
     *
     * AND READ ONLY WHEN THE FILE HAS CHANGED - design/47 B6. The
     * Status page asks every 2 s and this parsed the whole file each
     * time for one line that changes only when a plan runs. Its size
     * and mtime are the key; a plan rewrites the file, which moves
     * both (mtime is to the second on ext2, hence the size as well).
     */
    {
        static char   saved[VLHE_PATH_MAX];
        static time_t key_mtime = (time_t) -1;
        static off_t  key_size  = (off_t) -1;
        struct stat   ss;
        int           have = stat(vlhe_session_path(), &ss) == 0;

        if (!have || ss.st_mtime != key_mtime || ss.st_size != key_size) {
            int n;
            struct vlhe_sess_rec *rec = vlhe_session_read_all(&n);
            int i;

            saved[0] = '\0';
            for (i = n - 1; i >= 0; i--) {
                if (rec[i].kind != VLHE_SESS_LINK
                    || (rec[i].state != VLHE_SESS_OPEN
                        && rec[i].state != VLHE_SESS_FAILED)  /* retried */
                    || strcmp(rec[i].what, "/dev/cdrom") != 0)
                    continue;
                if (strncmp(rec[i].prior, "link ", 5) == 0) {
                    strncpy(saved, rec[i].prior + 5, sizeof saved - 1);
                    saved[sizeof saved - 1] = '\0';
                }
                break;          /* the most recent one wins */
            }
            free(rec);
            key_mtime = have ? ss.st_mtime : (time_t) -1;
            key_size  = have ? ss.st_size  : (off_t) -1;
        }
        strncpy(out->saved, saved, sizeof out->saved - 1);
        out->saved[sizeof out->saved - 1] = '\0';
    }

    if (lstat("/dev/cdrom", &sb) != 0)
        return 0;                       /* absent */

    if (S_ISLNK(sb.st_mode)) {
        out->kind = VLHE_CDROM_SYMLINK;
        n = readlink("/dev/cdrom", out->target, sizeof out->target - 1);
        if (n > 0)
            out->target[n] = '\0';      /* readlink does NOT terminate */
        else
            out->target[0] = '\0';

        /* IS IT OURS? Matched on the name rather than by resolving
         * the target, because the point is whether WE pointed it
         * there - and after an rmmod the node may not resolve at
         * all. */
        out->is_ours = strncmp(out->target, "/dev/vdisc", 10) == 0;
        /* OR THROUGH OUR INNER LINK - /dev/cdrom -> <run dir>/cdrom ->
         * /dev/vdiscN since 2026-10-03, so the CD page can move it
         * live. `via' is the drive, for the Status page to name. */
        if (strcmp(out->target, vdiscd_ctl_cdrom()) == 0) {
            out->is_ours = 1;
            n = readlink(out->target, out->via, sizeof out->via - 1);
            out->via[n > 0 ? n : 0] = '\0';
        }
        return 0;
    }

    if (S_ISBLK(sb.st_mode) || S_ISCHR(sb.st_mode)) {
        /*
         * A REAL DEVICE NODE - REFUSED, NOT REPLACED, and the
         * refusal names a way forward.
         *
         * THE USER'S CALL: "check if symlink refuse, maybe check for
         * other symlinks like /dev/cdrom0 or something and report
         * that free". Deleting a real node and recreating it later is
         * a different risk class from re-pointing a link, and a
         * machine whose /dev/cdrom IS the drive is one where somebody
         * chose that.
         *
         * THE `1' SUFFIX IS COREL'S OWN IDIOM - its mount point is
         * /mnt/cdrom1 and its automounter map is auto.cdrom1 - so a
         * name in that series will not look foreign on this machine.
         */
        int i;

        out->kind = VLHE_CDROM_NODE;
        for (i = 0; i < 10; i++) {
            char cand[64];

            sprintf(cand, "/dev/cdrom%d", i);
            if (lstat(cand, &sb) != 0) {
                strcpy(out->free_name, cand);
                break;
            }
        }
        return 0;
    }

    out->kind = VLHE_CDROM_OTHER;
    return 0;
}

/*
 * THE MAJOR vdisc IS RUNNING AT, from /proc/devices - the same file
 * major_in_use() reads, looked at from the other side: the line NAMED
 * "vdisc" in the Block section. -1 when there is none, which with the
 * module loaded cannot happen. design/47 B2, 2026-09-30: `major_applied'
 * had never been assigned, so the Options tab compared the configured
 * major against 0 and said "Reload vdisc to apply" for ever.
 */
static int
vdisc_running_major(void)
{
    FILE *fp;
    char line[128];
    int in_block = 0, found = -1;

    {
        const char *v = getenv("VLHE_PROC_DEVICES");
        fp = fopen((v != NULL && *v != '\0') ? v : "/proc/devices", "r");
    }
    if (fp == NULL)
        return -1;
    while (fgets(line, sizeof line, fp) != NULL) {
        int n;
        char name[64];

        if (strncmp(line, "Block", 5) == 0) {
            in_block = 1;
            continue;
        }
        if (strncmp(line, "Character", 9) == 0) {
            in_block = 0;
            continue;
        }
        if (!in_block)
            continue;
        if (sscanf(line, "%d %63s", &n, name) == 2
            && strcmp(name, "vdisc") == 0) {
            found = n;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Is this major on the impersonation list? `mimic' is derived from it
 * rather than stored - design/47 C2: the checkbox was never persisted,
 * so after a restart with Major=25 the ordinary-major menu showed 60
 * and the next OK wrote 60. */
static int
major_is_mimic(int num)
{
    struct vlhe_major mj[VLHE_MAX_MAJORS];
    int n, k;

    n = vlhe_majors(VLHE_MAJ_MIMIC, mj, VLHE_MAX_MAJORS);
    for (k = 0; k < n; k++)
        if (mj[k].num == num)
            return 1;
    return 0;
}

int
vlhe_modopts(struct vlhe_modopts *out)
{
    load_config();
    memset(out, 0, sizeof *out);

    out->major  = vlhe_conf_get_int(&cfg_sys, "CD Settings", "Major", 63);
    out->ndevs  = vlhe_conf_get_int(&cfg_sys, "CD Settings", "Drives", 1);
    out->packet = vlhe_conf_get_int(&cfg_sys, "CD Settings", "Packet", 1);
    /* OFF BY DEFAULT - it REDIRECTS a link that already works and
     * already points at the user's real drive (measured on
     * tests/vm/fresh.img: /dev/cdrom -> /dev/hdb). See the header. */
    out->link_cdrom = vlhe_conf_get_int(&cfg_sys, "CD Settings",
                                        "LinkCdrom", 0);
    /* THE PLAN'S OWN READER, so the default (on) has one home. */
    out->drives_autoload = vlhe_drives_autoload();

    /* WHAT THE RUNNING MODULE HAS, which is not what the file says
     * whenever the file has been edited since the last load. 2.2 has
     * no /sys, so a module's parameters cannot be read back - the
     * drive COUNT can be counted, and the major cannot be recovered
     * at all without asking the module. */
    out->mimic  = major_is_mimic(out->major);
    out->loaded = vlhe_status_module_loaded("vdisc");
    out->ndevs_applied = vlhe_drive_count();
    /* B2: the RUNNING major, read back; packet cannot be. */
    out->major_applied = out->loaded ? vdisc_running_major() : 0;
    if (out->major_applied < 0)
        out->major_applied = 0;
    out->packet_applied = -1;
    return 0;
}

/*
 * SET A MACHINE KEY ONLY IF ITS VALUE MOVES - design/49 T0, fix 3.
 *
 * vlhe_conf_set() already leaves a key alone when it is set to what
 * the file holds. An ABSENT key was the gap: every page's collect
 * pushes its whole struct, so with no config each untouched default
 * counted as a change, the system half went dirty, and a Save of a
 * font and a folder created the system file. `cur' is the value the
 * GETTER reports - the file's if present, the default if not - so the
 * defaults are never restated here and cannot drift from the getters.
 */
static void
sys_set_int(const char *section, const char *key, int v, int cur)
{
    if (v != cur)
        vlhe_conf_set_int(&cfg_sys, section, key, v);
}

static void
sys_set(const char *section, const char *key, const char *v,
        const char *cur)
{
    if (strcmp(v, cur) != 0)
        vlhe_conf_set(&cfg_sys, section, key, v);
}

int
vlhe_set_modopts(const struct vlhe_modopts *in)
{
    load_config();

    /*
     * ONLY WHAT THE PAGE OFFERS - design/49 T0, and design/47 L4. The
     * major was 0-255; 0 asks the kernel for a dynamic major, and then
     * make_nodes() makes no node and the unload sweep finds none. The
     * CD page offers the two lists vlhe_majors() gives and nothing
     * else, so that is what a setting may be - which Load
     * Configuration relies on, since it checks through these setters.
     * in_use is not refused: the page offers those too, marked, and
     * our own loaded vdisc is one of them.
     */
    {
        struct vlhe_major mj[VLHE_MAX_MAJORS];
        int w, k, n, found = 0;

        for (w = VLHE_MAJ_LOCAL; w <= VLHE_MAJ_MIMIC && !found; w++) {
            n = vlhe_majors(w, mj, VLHE_MAX_MAJORS);
            for (k = 0; k < n; k++)
                if (mj[k].num == in->major)
                    found = 1;
        }
        if (!found)
            return -1;
    }
    if (in->ndevs < VLHE_MIN_NDEVS || in->ndevs > VLHE_MAX_NDEVS)
        return -1;
    /* SWITCHES ARE 0 OR 1 - a page's toggle cannot produce anything
     * else, and a file saying 2 is refused rather than read as on. */
    if ((in->packet != 0 && in->packet != 1)
        || (in->link_cdrom != 0 && in->link_cdrom != 1)
        || (in->drives_autoload != 0 && in->drives_autoload != 1))
        return -1;

    {
        struct vlhe_modopts cur;

        memset(&cur, 0, sizeof cur);
        (void) vlhe_modopts(&cur);
        sys_set_int("CD Settings", "Major", in->major, cur.major);
        sys_set_int("CD Settings", "Drives", in->ndevs, cur.ndevs);
        sys_set_int("CD Settings", "Packet", in->packet ? 1 : 0,
                    cur.packet ? 1 : 0);
        sys_set_int("CD Settings", "LinkCdrom", in->link_cdrom ? 1 : 0,
                    cur.link_cdrom ? 1 : 0);
        sys_set_int("CD Settings", "DrivesAutoLoad",
                    in->drives_autoload ? 1 : 0,
                    cur.drives_autoload ? 1 : 0);
    }

    /* MEMORY ONLY - vlhe_commit() writes. See the header's Committing
     * section: a form's edits must not reach the file until Apply. */
    return 0;
}

int
vlhe_can_administer(void)
{
    /* ROOT AND THEN WRITABILITY - design/49 T0 narrows design/33
     * section 2, which decided by writability alone so that an admin
     * group owning the file would do. Only root ever acts on this
     * file, and a plan now refuses one another group can write, so a
     * non-root admin had nothing to administer. */
    return system_conf_savable();
}

/* ------------------------------------------------------------------ */
/* Status - every module and daemon                                   */
/* ------------------------------------------------------------------ */

int
vlhe_components(struct vlhe_component *out, int max)
{
    static const struct {
        const char *name;
        int kind;
    } comps[] = {
        { "vsound",  VLHE_COMP_MODULE },
        { "vdisc",   VLHE_COMP_MODULE },
        { "vmidi",   VLHE_COMP_MODULE },
        { "vsoundd", VLHE_COMP_DAEMON },
        { "vdiscd",  VLHE_COMP_DAEMON },
        { "vmidid",  VLHE_COMP_DAEMON }
    };
    int i, n = 0;

    for (i = 0; i < (int)(sizeof comps / sizeof comps[0]) && n < max; i++) {
        memset(&out[n], 0, sizeof out[n]);
        strncpy(out[n].name, comps[i].name, sizeof out[n].name - 1);
        out[n].kind = comps[i].kind;

        if (comps[i].kind == VLHE_COMP_MODULE) {
            int users;

            out[n].present = vlhe_status_module_loaded(comps[i].name);
            users = vlhe_status_module_users(comps[i].name);
            if (out[n].present && users >= 0) {
                /*
                 * "IN USE BY N", NOT "N USERS" - the user's point,
                 * 2026-09-18: "what is a user someone might ask".
                 *
                 * The number was never wrong. It is /proc/modules'
                 * reference count, which for vsound is one per OPEN
                 * FILE DESCRIPTOR: every playing client
                 * (vsound_dev.c:1007), the pump's read fd (:917),
                 * and this GUI while it polls. Doom, quake, a synth,
                 * the pump and the control centre is five, and all
                 * five are real.
                 *
                 * BUT "5 users" IS lsmod's VOCABULARY, answering
                 * "can this be unloaded" while reading like "five
                 * things are making sound". They are different
                 * questions and the page was answering the wrong one.
                 *
                 * "in use by 5" says the same thing about references
                 * without implying they are people or programs.
                 */
                sprintf(out[n].detail, "in use by %d", users);
            } else if (out[n].present) {
                /* Loaded, and the count could not be read - say so
                 * rather than leaving the cell blank, which reads as
                 * "nothing is using it". */
                strcpy(out[n].detail, "loaded");
            }
            /* vmidi's row carries the one number lxdoom needs - row 62 */
            if (out[n].present && strcmp(comps[i].name, "vmidi") == 0) {
                int sd = vlhe_midi_seqdev();

                if (sd >= 0) {
                    size_t len = strlen(out[n].detail);
                    sprintf(out[n].detail + len, "%sMIDI device %d",
                            len ? " - " : "", sd);
                }
            }
            /* A MODULE GETS NO BUTTON AT ALL - the user's call,
             * 2026-09-17: "if they cant be restarted from that page do
             * they need a button at all". */
            out[n].can_restart = 0;
        } else {
            const char *pf = vlhe_status_pidfile_path(comps[i].name, -1);
            int st = vlhe_status_daemon(pf);

            if (st > 0) {
                out[n].present = 1;
                out[n].pid = st;
                sprintf(out[n].detail, "pid %d", st);
            } else if (st < 0) {
                /* THE THIRD STATE, and the useful one: a pid file
                 * naming a DEAD process says the daemon crashed
                 * rather than that it was never started. */
                out[n].present = 0;
                strcpy(out[n].detail, "stale pid file - it died");
            } else {
                out[n].present = 0;
                /* NO PID FILE MEANS NOT RUNNING - every daemon writes
                 * one at start (vlhe_status_write_pidfile) and removes
                 * it at a clean exit. Corrected 2026-10-03: this said
                 * none of them wrote one, true when design/09 finding 4
                 * was written and long since fixed. */
                strcpy(out[n].detail, "no pid file");
            }
            out[n].can_restart = 1;
        }
        n++;
    }
    return n;
}


/* ------------------------------------------------------------------ */
/* Sound - the pump and its module                                    */
/* ------------------------------------------------------------------ */

int
vlhe_sound(struct vlhe_sound *out)
{
    struct vsound_chanlist cl;

    load_config();
    memset(out, 0, sizeof *out);

    /* EMPTY BY DEFAULT, AND THAT MEANS DETECT - the same argument as
     * card_module below, and the same answer. This fell back to
     * "/dev/dsp1" until 2026-09-24, which was right while the key
     * meant "where the card ENDS UP"; under the new meaning it would
     * assert that the user's speakers are on dsp1, which is a guess
     * about the machine. Worse, a stale reader treating it as a
     * destination would get vsound's own node - the self-loop
     * design/38 exists to stop. */
    strncpy(out->card,
            vlhe_conf_get(&cfg_sys, "Sound Settings", "CardDevice", ""),
            sizeof out->card - 1);
    /* THE REDIRECTED NAME, defaulting to the one nearly everything
     * opens. Unlike `card' this has a real default rather than
     * meaning detect: there is nothing to detect, only a convention
     * to follow until the user says otherwise. */
    strncpy(out->programs_use,
            vlhe_conf_get(&cfg_sys, "Sound Settings", "ProgramsUse",
                          "/dev/dsp"),
            sizeof out->programs_use - 1);
    /* EMPTY BY DEFAULT, AND THAT MEANS DETECT - see vlhe_backend.h.
     * A default of "sb" would be a guess about the machine, and the
     * one thing this field exists to avoid is guessing. */
    strncpy(out->card_module,
            vlhe_conf_get(&cfg_sys, "Sound Settings", "CardModule", ""),
            sizeof out->card_module - 1);
    out->release_on_idle =
        vlhe_conf_get_int(&cfg_sys, "Sound Settings", "ReleaseOnIdle", 0);
    out->midi_slot =
        vlhe_conf_get_int(&cfg_sys, "Sound Settings", "MidiChannel", 1);
    /* THE MIX'S HEADROOM - design/09 "MIX HEADROOM", built
     * 2026-10-02. The limiter is ON by default (attack/release); the
     * fixed attenuation is OFF by default (100%). A value out of range
     * reads as the default, as the module itself would take it. */
    out->limiter =
        vlhe_conf_get_int(&cfg_sys, "Sound Settings", "Limiter", 1);
    if (out->limiter < 0 || out->limiter > 2)
        out->limiter = 1;
    out->attenuation =
        vlhe_conf_get_int(&cfg_sys, "Sound Settings", "Attenuation", 100);
    if (out->attenuation < 1 || out->attenuation > 100)
        out->attenuation = 100;

    out->loaded = vlhe_status_module_loaded("vsound");

    /*
     * THE CHANNEL COUNT IS A COMPILE-TIME CONSTANT AND IS ALWAYS
     * ANSWERED - vsound_chan.h's VSOUND_MAX_CHAN, 4. It does not
     * depend on anything being loaded, so it is filled in first.
     *
     * A FIRST VERSION LEFT IT 0 WHEN THE MODULE WAS ABSENT, and the
     * Sound page then read "0 programs can play at once" directly
     * above its own text saying "Four is set when the modules are
     * built". Caught by running the real backend on a machine with no
     * vsound, 2026-09-18 - which is what running it was for.
     */
    out->channels = VLHE_ORDINARY_CHANS;

    /*
     * WHAT THE RUNNING MODULE HAS. 2.2 cannot read a parameter back,
     * but the channel list says it anyway: nchan is VSOUND_SLOTS when
     * vsound_midi=1 and VSOUND_MAX_CHAN otherwise, and the reserved
     * slot carries VSOUND_CI_MIDI whether or not it is held
     * (vsound.h). So the applied value is derivable without a new
     * ioctl.
     */
    if (chanlist(&cl) == 0) {
        int i;

        for (i = 0; i < (int) cl.nchan; i++) {
            if (cl.chan[i].flags & VSOUND_CI_MIDI) {
                out->midi_slot_applied = 1;
                break;
            }
        }
    }

    /* THE PUMP: a daemon, so its pid file answers - vsoundd writes
     * one at startup, in the account's ctl/ directory (design/54 D44). */
    out->running = vlhe_status_daemon(
        vlhe_status_pidfile_path("vsoundd", -1)) > 0;
    return 0;
}

int
vlhe_set_sound(const struct vlhe_sound *in)
{
    load_config();

    /* WHAT THE PAGE'S TWO DEVICE LISTS CAN HOLD, and 0/1 switches -
     * design/49 T0. CardModule is not checked here: no page sets it,
     * so it only ever carries the file's own value through, and Load
     * Configuration never takes it. */
    if (!dsp_node_ok(in->card) || !dsp_node_ok(in->programs_use))
        return -1;
    if ((in->release_on_idle != 0 && in->release_on_idle != 1)
        || (in->midi_slot != 0 && in->midi_slot != 1)
        || in->limiter < 0 || in->limiter > 2
        || in->attenuation < 1 || in->attenuation > 100)
        return -1;

    {
        struct vlhe_sound cur;

        /* A getter that fails leaves `cur' zeroed, which compares as
         * "changed" for any value set - the old behaviour, not a loss. */
        memset(&cur, 0, sizeof cur);
        (void) vlhe_sound(&cur);
        sys_set("Sound Settings", "CardDevice", in->card, cur.card);
        sys_set("Sound Settings", "ProgramsUse", in->programs_use,
                cur.programs_use);
        sys_set("Sound Settings", "CardModule", in->card_module,
                cur.card_module);
        sys_set_int("Sound Settings", "ReleaseOnIdle",
                    in->release_on_idle ? 1 : 0,
                    cur.release_on_idle ? 1 : 0);
        sys_set_int("Sound Settings", "MidiChannel", in->midi_slot ? 1 : 0,
                    cur.midi_slot ? 1 : 0);
        sys_set_int("Sound Settings", "Limiter", in->limiter,
                    cur.limiter);
        sys_set_int("Sound Settings", "Attenuation", in->attenuation,
                    cur.attenuation);
    }

    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

/*
 * THE ARGUMENTS A MODULE IS GIVEN IN /etc/modules.
 *
 * The file's lines are `module' or `module arg=value ...', and the
 * suffix may or may not be there - CLAUDE.md section 3 has the real
 * files: six bare names and four with `.o' in one of them, and
 * `opl3 io=0x7080' showing the arguments live on the line.
 *
 * MATCHES WITH OR WITHOUT `.o', because which spelling a line carries
 * is whichever party last touched it - devicesupdate rewrites them
 * and the suffix follows /etc/devices.
 *
 * Fills `out' with what follows the name, empty when the line has
 * none or the module is not listed.
 */
static void
module_args(const char *name, char *out, size_t len)
{
    FILE *fp;
    char  line[512];
    const char *path;

    if (out == NULL || len == 0)
        return;
    out[0] = '\0';
    if (name == NULL)
        return;

    path = getenv("VLHE_ETC_MODULES");
    if (path == NULL || *path == '\0')
        path = "/etc/modules";

    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    while (fgets(line, sizeof line, fp) != NULL) {
        char *p = line;
        char *word;
        size_t nlen = strlen(name);

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;           /* a comment, or the `#auto' control */

        word = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n')
            p++;

        /* `es1371' or `es1371.o', and nothing longer. */
        if (strncmp(word, name, nlen) != 0)
            continue;
        if (word[nlen] != '\0' && (size_t)(p - word) != nlen &&
            strncmp(word + nlen, ".o", 2) != 0)
            continue;
        if ((size_t)(p - word) != nlen &&
            (size_t)(p - word) != nlen + 2)
            continue;

        while (*p == ' ' || *p == '\t')
            p++;

        /*
         * TRUNCATION WOULD BE WORSE THAN NOTHING HERE. A cut argument
         * list - `io=0x220 irq=5 dma=' - is not merely incomplete, it
         * would be passed to insmod and fail or, worse, set the wrong
         * thing. If the line does not fit, report none and let the
         * user type them: empty already means "we did not find any".
         */
        if (strlen(p) >= len) {
            out[0] = '\0';
            break;
        }

        strcpy(out, p);

        /* Trim the newline and any trailing space. */
        {
            size_t k = strlen(out);

            while (k > 0 && (out[k-1] == '\n' || out[k-1] == '\r' ||
                             out[k-1] == ' '  || out[k-1] == '\t'))
                out[--k] = '\0';
        }
        break;
    }

    fclose(fp);
}

/*
 * WHICH SOUND DRIVERS ARE LOADED - by MODULE name.
 *
 * vlhe_backend.h has why this is not vlhe_cards() and why the bracket
 * list is the only method that finds an out-of-tree or hand-built
 * driver.
 *
 * BOTH soundcore AND sound ARE ASKED. soundcore exports
 * register_sound_dsp, so every card driver holds it; `sound' is the
 * OSS core that vmidi needs and a legacy driver may hold that
 * instead. A name found twice is reported once.
 */
int
vlhe_sound_drivers(struct vlhe_driver *out, int max)
{
    static const char *const cores[] = { "soundcore", "sound" };
    /* OUR OWN MODULES ARE NOT THE USER'S CARD. They hold soundcore
     * exactly as a card driver does, so without this the dropdown
     * would offer vsound as something to unload and reload around
     * itself. */
    static const char *const ours[] = { "vsound", "vmidi", "vdisc" };
    char holders[VLHE_MAX_HOLDERS][64];
    int  n = 0;
    int  c;

    if (out == NULL || max <= 0)
        return 0;

    for (c = 0; c < 2; c++) {
        int got = vlhe_status_module_holders(cores[c], holders,
                                             VLHE_MAX_HOLDERS);
        int i;

        for (i = 0; i < got && n < max; i++) {
            int skip = 0;
            int k;

            for (k = 0; k < 3; k++)
                if (strcmp(holders[i], ours[k]) == 0)
                    skip = 1;
            for (k = 0; k < n; k++)
                if (strcmp(out[k].module, holders[i]) == 0)
                    skip = 1;    /* already seen via the other core */
            if (skip)
                continue;

            strncpy(out[n].module, holders[i], sizeof out[n].module - 1);
            out[n].module[sizeof out[n].module - 1] = '\0';
            out[n].users = vlhe_status_module_users(holders[i]);
            module_args(out[n].module, out[n].args, sizeof out[n].args);
            n++;
        }
    }

    return n;
}

int
vlhe_cards(struct vlhe_card *out, int max)
{
    /*
     * THE CARDS THIS MACHINE HAS. /proc/sound lists the LEGACY path's
     * devices only - CLAUDE.md section 5: a PCI driver registers its
     * own fops and never enters audio_devs[], so esssolo1, es1371 and
     * emu10k1 do not appear there at all. Measured on the fresh
     * install 2026-09-17: /proc/sound does not even exist with es1371
     * loaded.
     *
     * SO THE NODES ARE THE ONLY HONEST SOURCE. Probe /dev/dsp through
     * /dev/dsp3 and report what opens. A name is only available for
     * the ones /proc/sound does list.
     */
    int i, n = 0;
    int vsound_here = vlhe_status_module_loaded("vsound");

    /* HOW FAR TO PROBE - VLHE_DSP_PROBE_MAX, in vlhe_backend.h, where
     * the reasoning and the -D override are. Moved there 2026-09-24
     * because the plan needs the same number to predict where a card
     * lands after the shuffle (design/38). */

    for (i = 0; i < VLHE_DSP_PROBE_MAX && n < max; i++) {
        char node[VLHE_PATH_MAX];
        int fd;

        if (i == 0)
            strcpy(node, "/dev/dsp");
        else
            sprintf(node, "/dev/dsp%d", i);

        /*
         * BUSY IS A CARD; ENXIO IS NOT - design/47 B4, 2026-10-01. A
         * node whose open is refused because something holds it - the
         * pump after a Load, a game - is exactly the card the user
         * wants to see in the list, and it used to vanish. CLAUDE.md
         * section 5 (corrected 2026-10-04): the drivers answer EBUSY,
         * EWOULDBLOCK or EAGAIN when busy; a minor registered to
         * nothing answers ENODEV, from soundcore itself, and ENXIO comes
         * from a driver with nothing present. So ENODEV, ENXIO and a
         * missing node are "no card"; every other refusal is a card in
         * use. NOT HANDLED HERE: i810_audio and trident answer ENODEV
         * when every channel is busy, so such a card vanishes from this
         * list while busy - the probe in vlhe_apply.c asks /proc/modules
         * for that case and this does not (design/54 D65).
         */
        /* LOOKING MUST NOT LOAD A MODULE - CLAUDE.md section 5,
         * vlhe_modconf.h. An empty minor with an alias for it (or no
         * soundcore at all) would load a driver just because the list
         * was drawn; it is left out, as an empty minor is. */
        {
            struct stat ns;

            if (stat(node, &ns) == 0 && S_ISCHR(ns.st_mode)
                && (int) major(ns.st_rdev) == 14
                && vlhe_modconf_open_would_load((int) minor(ns.st_rdev),
                                                NULL, 0))
                continue;
        }
        fd = open(node, O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == ENXIO || errno == ENOENT || errno == ENODEV)
                continue;
        } else {
            close(fd);
        }

        memset(&out[n], 0, sizeof out[n]);
        out[n].busy = (fd < 0);
        /*
         * NOT strncpy - BOTH BUFFERS ARE VLHE_PATH_MAX, so copying
         * one into the other minus a byte is a truncation GCC is
         * right to warn about even though "/dev/dspN" can never
         * reach it. sizeof the SOURCE says what is actually being
         * copied and the assert-shaped check says why it fits.
         */
        if (strlen(node) >= sizeof out[n].node)
            continue;           /* cannot happen: "/dev/dspN" */
        strcpy(out[n].node, node);

        /*
         * OURS IS THE ONE vsound HOLDS - AND ONLY WHEN IT IS LOADED.
         *
         * A first version tested the NODE alone, so on 86Box before
         * load.sh it marked the real SB16's /dev/dsp as "this is
         * vsound" against a machine where vsound was not present.
         * Caught on target 2026-09-18. The node only becomes ours
         * once the module has taken it.
         */
        out[n].is_ours = vsound_here &&
                         strcmp(node, vsound_device()) == 0;

        /*
         * THE NAME IS LEFT EMPTY, DELIBERATELY.
         *
         * /proc/sound lists the LEGACY path only - a PCI driver
         * registers its own fops and never appears there
         * (CLAUDE.md section 5), and on the fresh install the file
         * does not exist at all. So there is no honest name to give
         * for most cards, and inventing one would be worse.
         *
         * AND THE MODULE ADDS ITS OWN MARK. vlhe_mod_sound.c:137
         * appends "(this is vsound)" when is_ours is set, so putting
         * the same words here printed them TWICE on target. The flag
         * is the interface; the wording belongs to the GUI.
         */
        out[n].name[0] = '\0';
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* MIDI - the synth daemon, its fonts and its module                  */
/* ------------------------------------------------------------------ */

int
vlhe_fonts(struct vlhe_font *out, int max)
{
    /* TWO SLOTS, NOT A LIST - the rework of 2026-09-17. render.h:493
     * has it from the corpus: 106 of 112 AWE32-era songs want "a GM
     * font in bank 0, the song's own in bank 1", so the general case
     * is one font and the interesting case is two. */
    int i, n = 0;

    load_config();
    for (i = 0; i < 2 && n < max; i++) {
        char key[32];
        const char *p;

        sprintf(key, "Font%d", i);
        p = vlhe_conf_get(&cfg_usr, "Midi Settings", key, "");
        if (p[0] == '\0')
            continue;

        memset(&out[n], 0, sizeof out[n]);
        strncpy(out[n].path, p, sizeof out[n].path - 1);
        out[n].bank = i;
        /* `ok' IS THE DAEMON'S ANSWER AND WE DO NOT HAVE IT. vmidid
         * reports what it loaded on its stderr, which goes to
         * DAEMON.LOG rather than anywhere we can read. Reporting the
         * file's existence instead would be a different claim wearing
         * the same field, so: 0 until the daemon can be asked. */
        out[n].ok = 0;
        n++;
    }
    return n;
}

static int g_test_root = -1;

void
vlhe_backend_test_root(int is_root)
{
    g_test_root = is_root;
}

/* THE REAL USER, not euid: the setuid build is euid 0 around its
 * privileged work while the person at the desk is not root, and their
 * fonts must not become the machine's. */
static int
real_user_is_root(void)
{
    return g_test_root >= 0 ? g_test_root : (getuid() == 0);
}

/* Fill `out' from Font<i> (or DefaultFont<i>) in `c'; returns the count. */
static int
fonts_from(const struct vlhe_conf *c, const char *prefix,
           struct vlhe_font *out, int max)
{
    int i, n = 0;

    for (i = 0; i < 2 && n < max; i++) {
        char key[32];
        const char *p;

        sprintf(key, "%s%d", prefix, i);
        p = vlhe_conf_get(c, "Midi Settings", key, "");
        if (p[0] == '\0')
            continue;
        memset(&out[n], 0, sizeof out[n]);
        strncpy(out[n].path, p, sizeof out[n].path - 1);
        out[n].bank = i;
        out[n].ok = 0;
        n++;
    }
    return n;
}

int
vlhe_fonts_effective(struct vlhe_font *out, int max, int *from_machine)
{
    int n;

    load_config();
    if (from_machine != NULL)
        *from_machine = 0;
    n = fonts_from(&cfg_usr, "Font", out, max);
    if (n > 0)
        return n;
    n = fonts_from(&cfg_sys, "DefaultFont", out, max);
    if (n > 0 && from_machine != NULL)
        *from_machine = 1;
    return n;
}

int
vlhe_set_fonts(const struct vlhe_font *in, int n)
{
    int i;
    const char *up;

    load_config();
    for (i = 0; i < 2; i++) {
        char key[32], dkey[32];

        sprintf(key, "Font%d", i);
        sprintf(dkey, "DefaultFont%d", i);
        if (i < n && in[i].path[0] != '\0')
            vlhe_conf_set(&cfg_usr, "Midi Settings", key, in[i].path);
        else
            vlhe_conf_unset(&cfg_usr, "Midi Settings", key);

        /* ROOT'S CHOICE IS ALSO THE MACHINE'S - design/54 D15. Mirrored
         * exactly, so clearing a slot as root clears the default too.
         * Memory-only like the rest: File > Save writes the system half
         * when it may (design/51). */
        if (real_user_is_root()) {
            if (i < n && in[i].path[0] != '\0')
                vlhe_conf_set(&cfg_sys, "Midi Settings", dkey, in[i].path);
            else
                vlhe_conf_unset(&cfg_sys, "Midi Settings", dkey);
        }
    }

    up = cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
    if (up == NULL)
        return -1;
    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Resetting settings                                                 */
/* ------------------------------------------------------------------ */

int
vlhe_is_root(void)
{
    return geteuid() == 0;
}

/* ------------------------------------------------------------------ */
/* Raising privilege around the work that needs it - vlhe_backend.h   */
/* ------------------------------------------------------------------ */

static int  (*g_priv_raise)(void);
static void (*g_priv_lower)(void);

void
vlhe_backend_set_priv(int (*raise)(void), void (*lower)(void))
{
    g_priv_raise = raise;
    g_priv_lower = lower;
}

/*
 * MAKE THE SYSTEM FILE ONE THE TRUST CHECK ACCEPTS - 2026-09-30, from
 * a target run. The writer creates the file through fopen() with the
 * process's umask, so a desktop umask of 002 gives 0664; and after
 * Modify only the effective UID is root - the gid is still the user's.
 * Root's own Save, made exactly as design/49 R1 intends, therefore
 * wrote a root:nova 0664 file, which the check rightly refused, and
 * root could not act on what it had just saved. Called inside the
 * raised window, after a successful write: mode 0644 always, owner
 * root:root when raised.
 */
static void
system_file_claim(const char *path)
{
    /* THE MODE ALWAYS - a config file must not inherit a wide umask
     * (a host test showed 0666 under umask 0). THE OWNER ONLY WHEN
     * RAISED: as a user chown fails, which is right, since that file
     * is not the system's. */
    (void) chmod(path, 0644);
    if (geteuid() == 0)
        (void) chown(path, 0, 0);
}

/*
 * CAN THIS SESSION BE ROOT FOR A WRITE? - design/49 T0.
 *
 * The same probe vlhe_reset_settings() uses: raise, ask, lower. In
 * the setuid build before Modify the raise does nothing, so this is
 * no; after Modify it is yes. In the ordinary build and the CLI there
 * are no hooks and it is geteuid(). THE SYSTEM CONFIG IS WRITTEN ONLY
 * WHEN THIS SAYS YES - in both builds, because only root ever acts on
 * that file, so a file a user wrote is one only root will use.
 *
 * The probe can be replaced for the host tests, which run as a user
 * and must still exercise the system half.
 */
static int (*g_root_probe)(void);

void
vlhe_backend_set_root_probe(int (*probe)(void))
{
    g_root_probe = probe;
}

static int
root_available(void)
{
    int ok;

    if (g_root_probe != NULL)
        return g_root_probe();
    vlhe_root_begin();
    ok = vlhe_is_root();
    vlhe_root_end();
    return ok;
}

int
vlhe_root_begin(void)
{
    return (g_priv_raise != NULL) ? g_priv_raise() : 0;
}

void
vlhe_root_end(void)
{
    /* errno SURVIVES THE LOWER. Every caller reads errno after the
     * privileged call to say what went wrong, and seteuid() is
     * allowed to overwrite it even when it succeeds. */
    int saved = errno;

    if (g_priv_lower != NULL)
        g_priv_lower();
    errno = saved;
}

/*
 * THE TWO CONTROL CHANNELS, RAISED. The FIFOs are 0660 under
 * /var/run/vlhe, so in the setuid build after Modify a request needs
 * root to reach the daemon at all.
 */
static int
vdisc_request(const char *req, char *reply, size_t max, int ms)
{
    int rc;

    vlhe_root_begin();
    rc = vdiscd_ctl_send(req, reply, max, ms);
    vlhe_root_end();
    /*
     * ANYTHING BUT A QUESTION FORGETS THE DRIVE ROWS - vlhe_drives()'s
     * cache. Listed by what DOES NOT change state, so a verb added
     * later forgets by default rather than leaving a stale row. After
     * the exchange, success or not: a timed-out attach may still have
     * happened.
     */
    if (strcmp(req, "status") != 0 && strcmp(req, "tracks") != 0
        && strncmp(req, "image ", 6) != 0 && strncmp(req, "toc ", 4) != 0)
        vlhe_drives_forget();
    return rc;
}

static int
vmidi_request(const char *req, char *reply, size_t max)
{
    int rc;

    vlhe_root_begin();
    rc = vmidid_ctl_request(req, reply, max);
    vlhe_root_end();
    return rc;
}

/* ------------------------------------------------------------------ */
/* Which components are enabled                                       */
/* ------------------------------------------------------------------ */

/* The section each one lives in. Indexed by VLHE_ENABLE_*. */
static const char *const enable_section[3] = {
    "Sound Settings",
    "Midi Settings",
    "CD Settings"
};

int
vlhe_component_at_boot(int which)
{
    if (which < 0 || which > 2)
        return 1;

    load_config();
    /*
     * DEFAULT 1 - see vlhe_backend.h. A config written before these
     * were honoured meant all three, and a machine that loaded
     * nothing after an upgrade would be the worse surprise.
     *
     * FROM cfg_sys, NOT cfg_usr: what gets loaded is a property of
     * the MACHINE, and on an installed one it is what the init script
     * will do at boot for every user.
     */
    return vlhe_conf_get_int(&cfg_sys, enable_section[which],
                             "LoadAtBoot", 1) != 0;
}

/*
 * DIAGNOSTIC TRACING - `[Tracing] Enabled', default 0. design/09's
 * "TRACING IS DEBUGGING AND SHOULD NOT SHIP AS IT IS" (the user,
 * 2026-09-01): off by default, reachable at runtime from the config or
 * the setup program, ONE setting - because a user's module cannot be
 * rebuilt for them. Built 2026-10-02.
 *
 * FROM cfg_sys, like LoadAtBoot: whether the modules trace is a
 * property of the machine, and on an installed one it is what the
 * init script's load does too.
 */
int
vlhe_mix_rate(void)
{
    int r;

    load_config();
    r = vlhe_conf_get_int(&cfg_sys, "Sound Settings", "MixRate", 0);
    return (r >= 8000 && r <= 48000) ? r : 0;
}

int
vlhe_midi_release_ms(void)
{
    int ms;

    load_config();
    ms = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "ChannelRelease",
                           3000);
    if (ms < 0)
        return 3000;
    return ms > 600000 ? 600000 : ms;
}

int
vlhe_drives_autoload(void)
{
    load_config();
    return vlhe_conf_get_int(&cfg_sys, "CD Settings", "DrivesAutoLoad", 1)
           != 0;
}

int
vlhe_tracing(void)
{
    int lvl;

    /* TWO LEVELS SINCE 2026-10-02 - the user: "Can we reduce some of
     * the tracing so what we need isnt hidden between other things
     * flooding". 1 = EVENTS: opens, closes, changes, refusals - what a
     * test reads. 2 = EVERYTHING: also vdisc's per-packet data reads
     * and vmidi's per-byte lines, which ran to 183,000 and 38,000
     * lines a run. Anything above 2 reads as 2. */
    load_config();
    lvl = vlhe_conf_get_int(&cfg_sys, "Tracing", "Enabled", 0);
    if (lvl < 0)
        lvl = 0;
    return lvl > 2 ? 2 : lvl;
}

/* [Boot] FinishLeftover - design/54 7h decision 2: the boot finishes a
 * load that was never unloaded. Default 1; only 0 turns it off. */
int
vlhe_boot_finish_leftover(void)
{
    load_config();
    return vlhe_conf_get_int(&cfg_sys, "Boot", "FinishLeftover", 1) != 0;
}

/* [Load] BaselineDrift - see vlhe_backend.h. Anything unrecognised
 * reads as "ask": asking is the safe answer to a value nobody chose. */
static const char *
bdrift_word(int m)
{
    return m == VLHE_BDRIFT_WARN ? "warn"
         : m == VLHE_BDRIFT_REFUSE ? "refuse" : "ask";
}

int
vlhe_baseline_drift(void)
{
    const char *v;

    load_config();
    v = vlhe_conf_get(&cfg_sys, "Load", "BaselineDrift", "ask");
    if (strcmp(v, "warn") == 0)
        return VLHE_BDRIFT_WARN;
    if (strcmp(v, "refuse") == 0 || strcmp(v, "abort") == 0)
        return VLHE_BDRIFT_REFUSE;
    return VLHE_BDRIFT_ASK;
}

int
vlhe_set_baseline_drift(int mode)
{
    if (mode < VLHE_BDRIFT_ASK || mode > VLHE_BDRIFT_REFUSE)
        return -1;
    load_config();
    sys_set("Load", "BaselineDrift", bdrift_word(mode),
            bdrift_word(vlhe_baseline_drift()));
    return 0;
}

/* [Tracing] Capture - the run-folder capture, off by default; only acts
 * with Enabled as well (vlhe_capture_begin()). */
int
vlhe_trace_capture(void)
{
    load_config();
    return vlhe_conf_get_int(&cfg_sys, "Tracing", "Capture", 0) ? 1 : 0;
}

int
vlhe_component_enabled(int which)
{
    if (which < 0 || which > 2)
        return 1;

    load_config();
    /* ITS OWN KEY, DEFAULTING TO LoadAtBoot - the split of
     * 2026-10-02 (vlhe_backend.h). Same half of the config: what the
     * control centre's Load does is still a property of the machine,
     * and a non-root user's Save puts it in the draft as before. */
    return vlhe_conf_get_int(&cfg_sys, enable_section[which],
                             "Include",
                             vlhe_component_at_boot(which) ? 1 : 0) != 0;
}

void
vlhe_set_component_enabled(int which, int on)
{
    if (which < 0 || which > 2)
        return;

    load_config();
    if ((on != 0) == vlhe_component_enabled(which))
        return;                 /* unchanged, default included - T0 */
    vlhe_conf_set(&cfg_sys, enable_section[which], "Include",
                  on ? "1" : "0");
}

/* ------------------------------------------------------------------ */
/* A portable copy's first run                                        */
/* ------------------------------------------------------------------ */

/*
 * COPY ONE FILE. Plain read/write rather than rename or link: the
 * source is somebody else's file that must not move, and the two may
 * be on different filesystems - a tarball unpacked on a CF card,
 * /etc on the hard disk.
 *
 * Returns 1 on success, 0 when the source is absent (not an error -
 * a machine may have no user config), -1 on a real failure.
 */
static int
copy_file(const char *from, const char *to)
{
    FILE *in, *out;
    char  buf[4096];
    size_t n;
    int    bad = 0;

    in = fopen(from, "rb");
    if (in == NULL)
        return 0;                       /* nothing to copy */

    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            bad = 1;
            break;
        }
    }
    if (ferror(in))
        bad = 1;

    fclose(in);
    /* fclose IS A WRITE - on a full disk it is often the only call
     * that can report, everything before it having been buffered.
     * The same lesson smf2wav learned the hard way on 2026-09-22. */
    if (fclose(out) != 0)
        bad = 1;

    if (bad) {
        remove(to);                     /* no half-copied config */
        return -1;
    }
    return 1;
}

int
vlhe_portable_offer_seed(void)
{
    char        path[VLHE_PATH_MAX];
    struct stat st;

    if (!vlhe_self_is_trial())
        return 0;                       /* not a portable copy */

    /*
     * IS THERE A CONFIG WORTH COPYING? That is the question this
     * dialog needs, and it is WEAKER than "was this machine
     * installed by our installer".
     *
     * IT USED TO ASK vlhe_conf_machine_installed(), WHICH COULD NEVER
     * BE TRUE - found on target 2026-09-23. That function tests for
     * `[Paths] ModuleDir' in /etc/vlhe.conf, and only `setup-vlhe'
     * was to write that key. (setup-vlhe exists now and still does not
     * write it - design/54 L05; installed-versus-portable is decided by
     * the PORTABLE marker instead. Corrected 2026-10-03, T13.) So the
     * marker is absent on every machine that exists, and a guest with a
     * perfectly good /etc/vlhe.conf full of settings was being told
     * it had nothing to offer.
     *
     * A READABLE /etc/vlhe.conf IS THE RIGHT TEST HERE. We are asking
     * whether to copy settings, not whether a package manager put
     * them there. vlhe_conf_machine_installed() stays correct for the
     * questions that genuinely mean INSTALLED - it is simply the
     * wrong one for this.
     */
    {
        FILE *fp = fopen("/etc/vlhe.conf", "r");

        if (fp == NULL)
            return 0;                   /* nothing to copy from */
        fclose(fp);
    }

    /* ALREADY ANSWERED. The tree has a config, so either the user
     * chose defaults and saved, or they chose to copy, or they have
     * been using it - and in every case the question is spent. */
    if (!vlhe_self_path("vlhe.conf", path, sizeof path))
        return 0;
    if (stat(path, &st) == 0)
        return 0;

    return 1;
}

int
vlhe_portable_seed(char *why, int max)
{
    char sys_to[VLHE_PATH_MAX];
    char usr_to[VLHE_PATH_MAX];
    const char *usr_from;
    int  n = 0, rc;

    if (why != NULL && max > 0)
        why[0] = '\0';

    if (!vlhe_self_is_trial()) {
        if (why != NULL)
            strncpy(why, "this is not a portable copy", max - 1);
        return -1;
    }

    /* THE USER FILE IS PER USER NOW - vlhe-user-<name>.conf, 2026-09-24
     * (vlhe_conf.h, the tiers). The seed copies root's ~/.vlhe file
     * to root's own slot in the tree, not to a file every user would
     * then share. */
    if (!vlhe_self_path("vlhe.conf", sys_to, sizeof sys_to)
        || vlhe_conf_user_candidate(VLHE_USER_TIER_DEFAULT) == NULL
        || strlen(vlhe_conf_user_candidate(VLHE_USER_TIER_DEFAULT))
           >= sizeof usr_to) {
        if (why != NULL)
            strncpy(why, "cannot tell where this copy lives", max - 1);
        return -1;
    }

    /* THE SYSTEM CONFIG. /etc/vlhe.conf by name rather than through
     * vlhe_conf_system_path(), which in a portable copy now RESOLVES
     * TO THE TREE - asking it here would copy the file onto itself. */
    strcpy(usr_to, vlhe_conf_user_candidate(VLHE_USER_TIER_DEFAULT));

    rc = copy_file("/etc/vlhe.conf", sys_to);
    if (rc < 0) {
        if (why != NULL)
            strncpy(why, "could not copy /etc/vlhe.conf", max - 1);
        return -1;
    }
    n += rc;

    /*
     * AND THE USER'S, from $HOME, for the same reason: the resolver
     * would answer with the tree. A machine may have none, which
     * copy_file reports as 0 rather than an error.
     */
    {
        static char from[VLHE_PATH_MAX];
        const char *home = getenv("HOME");

        usr_from = NULL;
        if (home != NULL && *home != '\0'
            && strlen(home) + sizeof "/.vlhe/vlhe.conf" < sizeof from) {
            sprintf(from, "%s/.vlhe/vlhe.conf", home);
            usr_from = from;
        }
    }
    if (usr_from != NULL) {
        rc = copy_file(usr_from, usr_to);
        if (rc < 0) {
            if (why != NULL)
                strncpy(why, "the system config was copied but yours"
                             " could not be", max - 1);
            return -1;
        }
        n += rc;
    }

    /* WHAT IS CACHED IS NOW WRONG - the files under it just changed,
     * and every getter reads the cache. The same reload
     * vlhe_reset_settings() does, for the same reason. */
    cfg_loaded = 0;
    load_config();

    return n;
}

const char *
vlhe_module_dir_conf(void)
{
    static char dir[VLHE_PATH_MAX];

    load_config();
    strncpy(dir, vlhe_conf_get(&cfg_sys, "Paths", "ModuleDir", ""),
            sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    return dir;
}

/*
 * BACK A FILE UP TO `<path>.old' AND REPLACE IT WITH THE DEFAULTS.
 *
 * The seed when there is one, the compiled-in template otherwise -
 * vlhe_backend.h says why. A file that does not exist is not an
 * error: resetting a machine that never had one leaves it with the
 * defaults, which is the right end state.
 */
static int
reset_one(const char *path, int which,
          void (*report)(const char *line))
{
    struct vlhe_conf cfg;
    char old[VLHE_PATH_MAX + 8];
    char line[VLHE_PATH_MAX + 80];
    int  had;

    if (path == NULL || *path == '\0')
        return 0;

    had = file_exists(path);

    if (had) {
        if (strlen(path) + 5 >= sizeof old) {
            if (report != NULL) {
                sprintf(line, "%.200s: path too long to back up", path);
                report(line);
            }
            return 0;
        }
        sprintf(old, "%s.old", path);
        /* RENAME, NOT COPY - it is atomic, it cannot half-succeed and
         * leave two truncated files, and it costs nothing on the same
         * filesystem, which a file and its own .old always are. */
        if (rename(path, old) != 0) {
            if (report != NULL) {
                sprintf(line, "%.200s: cannot back up - %.40s",
                        path, strerror(errno));
                report(line);
            }
            return 0;
        }
    }

    /*
     * THE SEED IF THERE IS ONE. Read into an empty config and written
     * back out through the template, so the result carries the
     * comments and the key order a hand-written file would - a user
     * who opens it after a reset should see the documented form, not
     * a bare list.
     */
    cfg.n = 0;
    cfg.dirty = 0;
    if (vlhe_seed_exists()) {
        char sw[VLHE_PATH_MAX + 64];

        /* THE SYSTEM FILE TAKES ONLY A SEED ROOT COULD HAVE WRITTEN -
         * design/49 T0. The reset goes ahead from the template. */
        if (!seed_read_for(&cfg, which == VLHE_TPL_SYSTEM, sw,
                           (int) sizeof sw)
            && sw[0] != '\0' && report != NULL) {
            sprintf(line, "seed not used for the system file: %.200s", sw);
            report(line);
        }
    }

    if (vlhe_conf_template_write(&cfg, path, which, 0) != 0) {
        if (report != NULL) {
            sprintf(line, "%.200s: could not write the defaults", path);
            report(line);
        }
        return 0;
    }

    if (report != NULL) {
        if (had)
            /*
             * 150 EACH, NOT 200, AND THE ARITHMETIC IS THE REASON.
             * `line' is VLHE_PATH_MAX + 80 = 336 bytes; two %.200s
             * plus the 22 literal characters between and after them
             * reach 422, so a pair of long paths overran it. Both
             * operands scale with VLHE_PATH_MAX - `old' is `path'
             * with a suffix - so bounding only one would not help.
             * 150 + 150 + 22 = 322 fits with room to spare.
             *
             * FOUND BY A CLEAN BUILD, 2026-09-25. It had been
             * invisible because an incremental `make' skips the
             * object and the warning with it, which is what
             * `check-warnings' now exists to stop.
             */
            sprintf(line, "%.150s reset - the old one is %.150s",
                    path, old);
        else
            sprintf(line, "%.200s written with the defaults", path);
        report(line);
    }
    return 1;
}

/*
 * ONE USER'S FILE, WRITTEN AS THAT USER - design/55 recommendation 5,
 * removing R4 (2026-10-04).
 *
 * Root used to do this itself: rename ~/.vlhe/vlhe.conf to .old, then
 * fopen(vlhe.conf.tmp, "w") in a directory the USER owns. Any user with
 * a config could put a symlink at that name, and root would truncate
 * and overwrite whatever it pointed at - /etc/shadow - with the
 * template. And a reset file was left owned by root, in the user's own
 * directory.
 *
 * NOW A CHILD BECOMES THE FILE'S OWNER - uid and gid from passwd, root's
 * supplementary groups dropped - and does the same work. A planted link
 * then reaches only what that user could write anyway, and the result
 * belongs to them. Checked after the switch with getuid()/geteuid(), so
 * a child that did not become the user writes nothing.
 *
 * THE CHILD MAY NOT REPORT ITSELF: the GUI's report callback is GTK code
 * (report_lowered, below). Its lines come back through a pipe and the
 * parent reports them. It ends with _exit(), so nothing of the parent's
 * - stdio buffers, GTK's atexit work - runs twice. Waited for by its
 * own saved pid; nothing in VLHE reaps with waitpid(-1).
 *
 * Returns 1 when the file was reset, 0 otherwise.
 */
static int g_reset_fd = -1;     /* the child's end of the pipe */

static void
report_to_parent(const char *line)
{
    size_t n = strlen(line);

    if (g_reset_fd < 0)
        return;
    if (write(g_reset_fd, line, n) != (ssize_t) n
        || write(g_reset_fd, "\n", 1) != 1)
        g_reset_fd = -1;        /* parent gone; nothing to tell */
}

static int
reset_one_as(const char *path, uid_t uid, gid_t gid,
             void (*report)(const char *line))
{
    char  line[VLHE_PATH_MAX + 80];
    char  buf[1024];
    int   fds[2];
    int   st = 0;
    pid_t pid;
    size_t have = 0;

    if (pipe(fds) != 0) {
        if (report != NULL) {
            sprintf(line, "%.200s: not reset - %.40s", path,
                    strerror(errno));
            report(line);
        }
        return 0;
    }

    pid = fork();
    if (pid < 0) {
        if (report != NULL) {
            sprintf(line, "%.200s: not reset - %.40s", path,
                    strerror(errno));
            report(line);
        }
        close(fds[0]);
        close(fds[1]);
        return 0;
    }

    if (pid == 0) {
        close(fds[0]);
        g_reset_fd = fds[1];

        /* ROOT'S GROUPS FIRST, then the gid, then the uid - the order
         * the switch must happen in, since after setuid() the others
         * are no longer permitted. setgroups() needs root; a host test
         * running as itself has no extra groups to drop. */
        if ((geteuid() == 0 && setgroups(0, NULL) != 0)
            || setgid(gid) != 0 || setuid(uid) != 0
            || getuid() != uid || geteuid() != uid
            || getgid() != gid || getegid() != gid) {
            sprintf(line, "%.200s: not reset - could not become its"
                          " owner (uid %ld)", path, (long) uid);
            report_to_parent(line);
            _exit(2);
        }
        _exit(reset_one(path, VLHE_TPL_USER, report_to_parent) ? 0 : 1);
    }

    /* THE PARENT: every line the child sends, then its verdict. */
    close(fds[1]);
    for (;;) {
        ssize_t r = read(fds[0], buf + have, sizeof buf - 1 - have);
        char   *nl;

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        have += (size_t) r;
        buf[have] = '\0';
        while ((nl = strchr(buf, '\n')) != NULL) {
            *nl = '\0';
            if (report != NULL)
                report(buf);
            have -= (size_t) (nl + 1 - buf);
            memmove(buf, nl + 1, have + 1);
        }
        if (have == sizeof buf - 1) {   /* a line too long: say it */
            if (report != NULL)
                report(buf);
            have = 0;
        }
    }
    if (have > 0 && report != NULL)
        report(buf);
    close(fds[0]);

    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/*
 * EVERY USER'S COPY. /home is walked rather than /etc/passwd parsed,
 * deliberately: passwd carries system accounts with home directories
 * that exist (/var/spool/news and friends), and a reset has no
 * business creating ~/.vlhe for a daemon account that never ran the
 * program. A directory under /home holding one is a person's.
 */
static int
reset_all_users(void (*report)(const char *line))
{
    FILE *fp;
    char  line[512];
    int   n = 0;

    /*
     * /etc/passwd, NOT opendir("/home") - corrected 2026-09-22 at the
     * user's question, "reset_all_users should check /root?".
     *
     * YES, AND IT COULD NOT. /root is not under /home, so the one
     * account most likely to have settings on these machines was the
     * one account this scope skipped - and on a project where nearly
     * everything runs as root, "all users" excluding root is wrong on
     * its own terms.
     *
     * READING passwd FIXES MORE THAN /root. A home under /usr/local,
     * an account whose directory does not match its name, a machine
     * that keeps homes somewhere else entirely - a directory listing
     * guesses at all of these and the password file states them.
     *
     * READ BY HAND rather than with getpwent(), for the same reason
     * installed_marker() reads its own file: this is called from
     * inside the backend and the format is seven colon-separated
     * fields. getpwent would also pull in NSS, which on a machine
     * with a network name service can block.
     */
    /* VLHE_PASSWD IS FOR THE HOST TEST, as vlhe_conf.c's is; root never
     * sees it (vlhe_self_root_env(), design/55 recommendation 3). */
    {
        const char *pw = getenv("VLHE_PASSWD");

        fp = fopen(pw != NULL && pw[0] != '\0' ? pw : "/etc/passwd", "r");
    }
    if (fp == NULL)
        return 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        char  path[VLHE_PATH_MAX];
        char *f[7];
        char *p = line;
        int   i;

        /* name:passwd:uid:gid:gecos:HOME:shell */
        for (i = 0; i < 7; i++) {
            f[i] = p;
            p = strchr(p, ':');
            if (p == NULL)
                break;
            *p++ = '\0';
        }
        if (i < 6 || f[5] == NULL || f[5][0] != '/')
            continue;               /* short line, or no real home */

        /* THE TRAILING NEWLINE if the shell field ran to the end. */
        {
            char *nl = strchr(f[6] != NULL ? f[6] : f[5], '\n');
            if (nl != NULL)
                *nl = '\0';
        }

        if (strlen(f[5]) + sizeof "/.vlhe/vlhe.conf" >= sizeof path)
            continue;
        sprintf(path, "%s/.vlhe/vlhe.conf", f[5]);

        /* ONLY WHERE ONE ALREADY EXISTS. Writing defaults into the
         * home of someone who has never run this program would
         * create a file they did not ask for, in a directory we were
         * only passing through.
         *
         * IT DOES NOT DEDUPLICATE, and that is worth knowing rather
         * than claiming otherwise: several accounts can share a home
         * - `/' is common for daemon users - and if that home has a
         * config, each line resets it again, the second backing up
         * what the first just wrote. Harmless (the result is the same
         * defaults) and untidy (the .old is then a default file).
         * Not fixed because the fix is a seen-list and no machine
         * here has two accounts sharing a home with VLHE settings in
         * it. */
        if (!file_exists(path))
            continue;

        /* AS THE ACCOUNT THAT OWNS THE HOME - see reset_one_as(). A
         * line whose uid or gid is not a plain number is skipped rather
         * than guessed at; root would otherwise be the fallback. */
        {
            char *e1, *e2;
            long  uid = strtol(f[2], &e1, 10);
            long  gid = strtol(f[3], &e2, 10);

            if (*f[2] == '\0' || *e1 != '\0' || uid < 0
                || *f[3] == '\0' || *e2 != '\0' || gid < 0)
                continue;
            n += reset_one_as(path, (uid_t) uid, (gid_t) gid, report);
        }
    }

    fclose(fp);
    return n;
}

int
vlhe_reset_all_users(void (*report)(const char *line))
{
    return reset_all_users(report);
}

/*
 * THE GUI'S REPORT CALLBACK, CALLED LOWERED. Reset reports each file
 * as it goes, and the GUI's callback sets a status label - GTK code.
 * Raised work must never run GTK, so the callback steps out of the
 * window for the duration. Correct only at the outermost level,
 * which is where reset is called from.
 */
static void (*g_reset_report)(const char *line);

static void
report_lowered(const char *line)
{
    vlhe_root_end();
    if (g_reset_report != NULL)
        g_reset_report(line);
    vlhe_root_begin();
}

int
vlhe_reset_settings(int scopes, void (*report)(const char *line))
{
    int n = 0;

    /*
     * RAISE FIRST, THEN ASK. vlhe_is_root() means "euid 0 now", and
     * in the setuid build after Modify that is true only inside a
     * vlhe_root_begin() window - so the question has to be asked
     * from inside one or it answers no for a user who has unlocked.
     */
    if (scopes & (VLHE_RESET_SYSTEM | VLHE_RESET_ALLUSERS)) {
        int ok;

        vlhe_root_begin();
        ok = vlhe_is_root();
        vlhe_root_end();
        if (!ok)
            return -1;
    }

    g_reset_report = report;

    if (scopes & VLHE_RESET_SYSTEM) {
        vlhe_root_begin();
        n += reset_one(cfg_sys_path, VLHE_TPL_SYSTEM,
                       report != NULL ? report_lowered : NULL);
        vlhe_root_end();
    }

    /*
     * THE CURRENT USER'S FILE IS RESET AS THE USER - design/49 N9.
     * It used to run at whatever euid the session held, which after
     * Modify was 0, and left the user's own config owned by root.
     */
    if (scopes & VLHE_RESET_USER)
        n += reset_one(cfg_usr_path, VLHE_TPL_USER, report);

    /* OTHER USERS' FILES: root reads passwd and starts the work, and
     * each file is written by a child that has become its owner -
     * design/55 recommendation 5 (R4), reset_one_as(). Raised so the
     * child can switch to any account. */
    if (scopes & VLHE_RESET_ALLUSERS) {
        vlhe_root_begin();
        n += reset_all_users(report != NULL ? report_lowered : NULL);
        vlhe_root_end();
    }

    g_reset_report = NULL;

    /* THE CACHE IS NOW WRONG - the files under it have changed, and
     * every getter reads from it. Forcing a reload is what makes the
     * GUI show the defaults without being restarted. */
    cfg_loaded = 0;
    load_config();

    return n;
}

int
vlhe_render(struct vlhe_render *out)
{
    if (out == NULL)
        return -1;

    load_config();
    memset(out, 0, sizeof *out);

    /* FROM cfg_usr, like every other per-user setting - the same
     * file the Interface block lives in. */
    out->use_vlhe = vlhe_conf_get_int(&cfg_usr, "Render Settings",
                                      "UseVlheSettings", 1);
    out->voices   = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Voices", 512);
    out->rate     = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Rate", 44100);
    out->gain_milli = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Gain", 500);
    out->law      = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Law",
                                      VLHE_LAW_SPEC);
    out->filter   = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Filter",
                                      VLHE_VF_AWE);
    /* REVERB AND CHORUS APART since 2026-10-05; the old `Effects' key
     * is no longer read (design/54 S21, the migration, deferred). */
    out->reverb   = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Reverb", 1);
    out->chorus   = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Chorus", 1);
    out->bypass   = vlhe_conf_get_int(&cfg_usr, "Render Settings", "Bypass", 0);
    out->modenv   = vlhe_conf_get_int(&cfg_usr, "Render Settings", "ModEnv",
                                      VLHE_MODENV_FAST)
                    == VLHE_MODENV_FAST ? VLHE_MODENV_FAST
                                        : VLHE_MODENV_REFERENCE;

    strncpy(out->gm_font,
            vlhe_conf_get(&cfg_usr, "Render Settings", "GmFont", ""),
            sizeof out->gm_font - 1);
    strncpy(out->song_font,
            vlhe_conf_get(&cfg_usr, "Render Settings", "SongFont", ""),
            sizeof out->song_font - 1);
    strncpy(out->lame,
            vlhe_conf_get(&cfg_usr, "Render Settings", "Lame", ""),
            sizeof out->lame - 1);
    out->wav_temp = vlhe_conf_get_int(&cfg_usr, "Render Settings",
                                      "WavTemp", VLHE_WAVTEMP_AUTO);

    /* CLAMPED ON THE WAY OUT, so a hand-edited file cannot produce a
     * command line smf2wav will refuse. Its own limits. */
    if (out->voices < 1 || out->voices > VLHE_RENDER_VOICES_MAX)
        out->voices = 512;
    if (out->gain_milli < 1 || out->gain_milli > 2000)
        out->gain_milli = 500;
    if (out->law < 0 || out->law > VLHE_LAW_LINEAR)
        out->law = VLHE_LAW_SPEC;
    if (out->filter < 0 || out->filter > VLHE_VF_NONE)
        out->filter = VLHE_VF_AWE;
    if (out->wav_temp < VLHE_WAVTEMP_AUTO || out->wav_temp > VLHE_WAVTEMP_BESIDE)
        out->wav_temp = VLHE_WAVTEMP_AUTO;

    return 0;
}

int
vlhe_set_render(const struct vlhe_render *in)
{
    const char *up;

    if (in == NULL)
        return -1;

    load_config();
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "UseVlheSettings",
                      in->use_vlhe ? 1 : 0);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Voices", in->voices);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Rate", in->rate);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Gain",
                      in->gain_milli);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Law", in->law);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Filter", in->filter);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Reverb",
                      in->reverb ? 1 : 0);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Chorus",
                      in->chorus ? 1 : 0);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "Bypass",
                      in->bypass ? 1 : 0);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "ModEnv",
                      in->modenv == VLHE_MODENV_FAST ? VLHE_MODENV_FAST
                                                     : VLHE_MODENV_REFERENCE);
    vlhe_conf_set(&cfg_usr, "Render Settings", "GmFont", in->gm_font);
    vlhe_conf_set(&cfg_usr, "Render Settings", "SongFont",
                  in->song_font);
    vlhe_conf_set(&cfg_usr, "Render Settings", "Lame", in->lame);
    vlhe_conf_set_int(&cfg_usr, "Render Settings", "WavTemp",
                      in->wav_temp);

    /*
     * MEMORY ONLY - vlhe_commit() writes, like every other setter on
     * this page's neighbours.
     *
     * A FIRST VERSION WROTE IMMEDIATELY and returned -1 when there
     * was no config file to write, which made render_collect() fail
     * - and do_commit() jumps to the first page whose collect()
     * refuses. So pressing OK ANYWHERE landed on Render MIDI, on a
     * machine with no config. Found on 86Box 2026-09-22; the user
     * had changed nothing on that page.
     *
     * The reasoning for writing here was that "these come from an
     * Apply or an OK, which is the commit point" - true of every
     * other setter too, and they all defer. Being the one that did
     * not is what made it the page that refused.
     */
    (void)up;
    return 0;
}

int
vlhe_font_dirs(char out[][VLHE_PATH_MAX], int max)
{
    /* USER PATHS FIRST, then the three defaults - so a font someone
     * placed deliberately wins over one of the same name in a system
     * directory (vlhe_backend.h). */
    int n = 0, i;
    const char *extra;
    const char *home = getenv("HOME");

    load_config();
    extra = vlhe_conf_get(&cfg_usr, "Fonts", "Search", "");

    /* Colon-separated, as the template's comment says. */
    while (*extra != '\0' && n < max) {
        const char *sep = strchr(extra, ':');
        size_t len = sep ? (size_t)(sep - extra) : strlen(extra);

        if (len > 0 && len < VLHE_PATH_MAX) {
            memcpy(out[n], extra, len);
            out[n][len] = '\0';
            n++;
        }
        if (!sep)
            break;
        extra = sep + 1;
    }

    for (i = 0; i < 3 && n < max; i++) {
        const char *d = vlhe_fontscan_defaults[i];

        if (d[0] == '~' && d[1] == '/') {
            if (home == NULL ||
                strlen(home) + strlen(d) >= VLHE_PATH_MAX)
                continue;
            sprintf(out[n], "%s%s", home, d + 1);
        } else {
            strncpy(out[n], d, VLHE_PATH_MAX - 1);
            out[n][VLHE_PATH_MAX - 1] = '\0';
        }
        n++;
    }
    return n;
}

int
vlhe_add_font_dir(const char *path)
{
    char cur[VLHE_CONF_VAL_MAX];
    const char *existing, *up;

    load_config();
    existing = vlhe_conf_get(&cfg_usr, "Fonts", "Search", "");

    /*
     * TWO REFUSALS THE GUI BELIEVED WERE HERE AND WERE NOT - 2026-09-24,
     * design/36 row 56. Its comment said "the backend refuses a
     * duplicate"; this function appended anything. So `./' from
     * /mnt/xfer added /mnt/xfer a second time, and `../' added /mnt -
     * a parent directory with no soundfont in it - as a search path.
     *
     * -2  ALREADY LISTED. Exact string after the caller's realpath(),
     *     and also one of the built-in defaults, which are searched
     *     whether or not they are listed.
     * -3  NO SOUNDFONT IN IT. A folder the user means to fill later is
     *     still refused - they can add it once it holds one, and a
     *     search path that finds nothing is only ever a surprise.
     */
    {
        const char *q = existing;
        size_t plen = strlen(path);
        int k;

        while (*q != '\0') {
            const char *e = strchr(q, ':');
            size_t len = e != NULL ? (size_t) (e - q) : strlen(q);

            if (len == plen && strncmp(q, path, len) == 0)
                return -2;
            if (e == NULL)
                break;
            q = e + 1;
        }
        for (k = 0; k < VLHE_N_FONTDIR_DEFAULTS; k++)
            if (strcmp(vlhe_fontscan_defaults[k], path) == 0)
                return -2;
    }
    {
        char one[1][VLHE_PATH_MAX];
        char found[1][VLHE_PATH_MAX];

        strncpy(one[0], path, VLHE_PATH_MAX - 1);
        one[0][VLHE_PATH_MAX - 1] = '\0';
        if (vlhe_fontscan((const char (*)[VLHE_PATH_MAX]) one, 1,
                          found, 1) == 0)
            return -3;
    }

    /*
     * THE LENGTH CHECK ABOVE MAKES THE CONCATENATION SAFE, and GCC
     * cannot see through it - it warns that "%s:%s" could write 508
     * bytes into 256. Building the string by hand rather than with
     * sprintf says the same thing in a form the compiler can follow,
     * so the warning goes rather than being annotated away.
     */
    if (strlen(existing) + strlen(path) + 2 > sizeof cur)
        return -1;
    if (existing[0] == '\0') {
        strcpy(cur, path);
    } else {
        size_t k = strlen(existing);

        memcpy(cur, existing, k);
        cur[k++] = ':';
        strcpy(cur + k, path);
    }

    vlhe_conf_set(&cfg_usr, "Fonts", "Search", cur);
    up = cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
    if (up == NULL)
        return -1;
    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

int
vlhe_remove_font_dir(const char *path)
{
    char cur[VLHE_CONF_VAL_MAX];
    const char *p, *up;
    size_t used = 0;

    load_config();
    p = vlhe_conf_get(&cfg_usr, "Fonts", "Search", "");
    cur[0] = '\0';

    /* THE DEFAULTS CANNOT BE REMOVED, only added to - the header says
     * so, and they are not in this string anyway. */
    while (*p != '\0') {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);

        if (!(len == strlen(path) && strncmp(p, path, len) == 0)) {
            if (used + len + 2 < sizeof cur) {
                if (used > 0)
                    cur[used++] = ':';
                memcpy(cur + used, p, len);
                used += len;
                cur[used] = '\0';
            }
        }
        if (!sep)
            break;
        p = sep + 1;
    }

    vlhe_conf_set(&cfg_usr, "Fonts", "Search", cur);
    up = cfg_usr_path[0] != '\0' ? cfg_usr_path : NULL;
    if (up == NULL)
        return -1;
    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

int
vlhe_available_fonts(char out[][VLHE_PATH_MAX], int max)
{
    char dirs[VLHE_MAX_FONTDIRS][VLHE_PATH_MAX];
    int nd = vlhe_font_dirs(dirs, VLHE_MAX_FONTDIRS);

    /* The cast is C89's: it will not implicitly convert char[][N] to
     * const char[][N], where later standards would. */
    return vlhe_fontscan((const char (*)[VLHE_PATH_MAX])dirs, nd,
                         out, max);
}

int
vlhe_synth(struct vlhe_synth *out)
{
    load_config();
    memset(out, 0, sizeof *out);

    out->voices = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Voices", 64);
    out->gain_milli =
        vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Gain", 500);
    /* EACH READ ON ITS OWN since 2026-10-05. Chorus used to be written
     * as a copy of Reverb and never read, so every existing file reads
     * back exactly as it played before. */
    out->reverb =
        vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Reverb", 1);
    out->chorus =
        vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Chorus", 1);
    out->rate = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Rate", 44100);
    out->law = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Law", 0);
    out->filter = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Filter", 0);
    /* ON BY DEFAULT, as TiMidity's is (design/21 section 16). */
    out->auto_voices =
        vlhe_conf_get_int(&cfg_sys, "Midi Settings", "AutoVoices", 1) ? 1 : 0;
    /* FAST UNLESS SET - design/54 D28: reference until the corpus
     * comparison showed the two identical (400 of 400 renders), then
     * fast, as agreed. */
    out->modenv = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "ModEnv",
                                    VLHE_MODENV_FAST)
                  == VLHE_MODENV_FAST ? VLHE_MODENV_FAST
                                      : VLHE_MODENV_REFERENCE;

    out->running = vlhe_status_daemon(
        vlhe_status_pidfile_path("vmidid", -1)) > 0;
    return 0;
}

/*
 * AUTOMATIC VOICE REDUCTION'S SIX TUNABLES - [Midi Settings]
 * AutoVoice*, 2026-10-02 (the user: "can any of that be tweakable or
 * set in a config? so i can adjust it and see what I like"). CONFIG
 * ONLY, no widget: tuning knobs, read by the plan (vmidid -a) and by
 * Apply (`set autovoice'), so an edit takes effect without a restart.
 *
 * CHECKED BY THE DAEMON'S OWN CODE - autovoice_set(), linked in - so
 * the ranges and the low < high < hard rule exist once. A refused set
 * gives the DEFAULTS and the reason; nothing is clamped.
 */
#if VLHE_AUTOVOICE_N != AUTOVOICE_NSET
#error "VLHE_AUTOVOICE_N (vlhe_backend.h) and AUTOVOICE_NSET (autovoice.h) differ"
#endif
static const char *const av_keys[VLHE_AUTOVOICE_N] = {
    "AutoVoiceDrain", "AutoVoiceEmergency", "AutoVoiceHealthy",
    "AutoVoiceSettle", "AutoVoiceFloor", "AutoVoiceTails"
};

int
vlhe_autovoice_settings(int v[VLHE_AUTOVOICE_N], int *changed,
                        const char **why)
{
    autovoice t;
    int def[VLHE_AUTOVOICE_N], i;
    const char *r;

    autovoice_init(&t, 1, 64);
    autovoice_get(&t, def);
    load_config();
    for (i = 0; i < VLHE_AUTOVOICE_N; i++)
        v[i] = vlhe_conf_get_int(&cfg_sys, "Midi Settings", av_keys[i],
                                 def[i]);
    r = autovoice_set(&t, v);
    if (why != NULL)
        *why = r;
    if (r != NULL) {
        for (i = 0; i < VLHE_AUTOVOICE_N; i++)
            v[i] = def[i];
        if (changed != NULL)
            *changed = 0;
        return -1;
    }
    if (changed != NULL) {
        *changed = 0;
        for (i = 0; i < VLHE_AUTOVOICE_N; i++)
            if (v[i] != def[i])
                *changed = 1;
    }
    return 0;
}

/* "drain=50,emergency=10,..." - vmidid's -a and `set autovoice'. */
void
vlhe_autovoice_spec(const int v[VLHE_AUTOVOICE_N], char *out)
{
    sprintf(out, "drain=%d,emergency=%d,healthy=%d,settle=%d,floor=%d,"
            "tails=%d", v[0], v[1], v[2], v[3], v[4], v[5]);
}

int
vlhe_set_synth(const struct vlhe_synth *in)
{
    load_config();

    if (in->voices < 1 || in->voices > VLHE_MAX_VOICES)
        return -1;
    /* THE PAGE'S 1-200 %, stored in thousandths - so tens only. */
    if (in->gain_milli < 10 || in->gain_milli > 2000
        || in->gain_milli % 10 != 0)
        return -1;
    /* AND THE THREE MENUS AND ONE SWITCH, as the page offers them -
     * design/49 T0. These were stored unchecked. */
    if (in->law != VLHE_LAW_SPEC && in->law != VLHE_LAW_LINEAR)
        return -1;
    if (in->filter < VLHE_VF_AWE || in->filter > VLHE_VF_NONE)
        return -1;
    {
        static const int rates[] = VLHE_SYNTH_RATES;    /* the page's */
        int k;

        for (k = 0; k < VLHE_SYNTH_NRATES && in->rate != rates[k]; k++)
            ;
        if (k == VLHE_SYNTH_NRATES)
            return -1;
    }
    if (in->reverb != 0 && in->reverb != 1)
        return -1;
    if (in->chorus != 0 && in->chorus != 1)
        return -1;
    if (in->auto_voices != 0 && in->auto_voices != 1)
        return -1;
    if (in->modenv != VLHE_MODENV_REFERENCE && in->modenv != VLHE_MODENV_FAST)
        return -1;

    {
        struct vlhe_synth cur;

        memset(&cur, 0, sizeof cur);
        (void) vlhe_synth(&cur);
        sys_set_int("Midi Settings", "Voices", in->voices, cur.voices);
        sys_set_int("Midi Settings", "AutoVoices", in->auto_voices,
                    cur.auto_voices);
        sys_set_int("Midi Settings", "Gain", in->gain_milli,
                    cur.gain_milli);
        /* TWO CHECKBOXES, TWO KEYS since 2026-10-05 - they moved as
         * one before. */
        sys_set_int("Midi Settings", "Reverb", in->reverb, cur.reverb);
        sys_set_int("Midi Settings", "Chorus", in->chorus, cur.chorus);
        sys_set_int("Midi Settings", "Law", in->law, cur.law);
        sys_set_int("Midi Settings", "Filter", in->filter, cur.filter);
        sys_set_int("Midi Settings", "Rate", in->rate, cur.rate);
        sys_set_int("Midi Settings", "ModEnv", in->modenv, cur.modenv);
    }
    /* RATE, WHICH THIS NEVER WROTE - design/36 row 90. `vlhe_synth()`
     * has read `Midi Settings/Rate' since the field was added, so the
     * read could only ever return the default: there was nothing to
     * store a value with. It is the most effective setting on a slow
     * machine - 795 xruns at 44100 against 1 at 22050 on 86Box. It is
     * set with the others above. */

    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

int
vlhe_midiopts(struct vlhe_midiopts *out)
{
    memset(out, 0, sizeof *out);
    load_config();

    out->minor = vlhe_conf_get_int(&cfg_sys, "Midi Settings", "Minor", 11);
    out->loaded = vlhe_status_module_loaded("vmidi");

    /* NOT READABLE. 2.2 has no /sys and vmidi exposes its minor
     * nowhere - it prints it at load and that is all. Left 0 rather
     * than echoing the wanted value back, which would make the two
     * fields agree whether or not the module matched. */
    /*
     * THE APPLIED MINOR, FROM THE NODE THAT EXISTS - 2026-09-24.
     *
     * This was `out->minor_applied = 0' - a placeholder - and the Midi
     * page compares minor against it to print "The module is running
     * on minor N. Reload vmidi to apply." So with vmidi loaded the
     * page would have said "running on minor 0" whatever the config
     * said, and design/36 row 57 could never pass. The honest answer
     * is the minor of /dev/vmidi as it stands: the plan creates that
     * node from the parameter it loaded the module with, so its minor
     * IS the applied one until the next load, whatever the config has
     * since become.
     */
    out->minor_applied = out->minor;
    if (out->loaded) {
        struct stat st;

        if (stat("/dev/vmidi", &st) == 0 && S_ISCHR(st.st_mode))
            out->minor_applied = (int) (st.st_rdev & 0xff);
    }

    /* AND WHICH MIDI DEVICE IT IS - row 62; -1 when it cannot be read. */
    out->seqdev = out->loaded ? vlhe_midi_seqdev() : -1;
    return 0;
}

/* Is one of the three dmfm drivers loaded, or named in /etc/modules
 * for the next boot? Either makes 15 a collision. No root: both files
 * are world-readable. */
static int
dmfm_driver_present(char *which, int max)
{
    static const char *drv[] = { "esssolo1", "cmpci", "sonicvibes" };
    int k;

    for (k = 0; k < 3; k++)
        if (vlhe_status_module_loaded(drv[k])) {
            strncpy(which, drv[k], (size_t) max - 1);
            which[max - 1] = '\0';
            return 1;
        }
    /* AND THE NEXT BOOT'S LIST - the Acer loads esssolo1 from
     * /etc/modules, so a session with the driver momentarily out
     * must not accept what the boot would then collide with. */
    {
        const char *path = getenv("VLHE_ETC_MODULES");
        FILE *fp;
        char line[256];

        if (path == NULL || *path == '\0')
            path = "/etc/modules";
        fp = fopen(path, "r");
        if (fp == NULL)
            return 0;
        while (fgets(line, sizeof line, fp) != NULL) {
            char *p = line;
            size_t n;

            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '#')
                continue;
            n = strcspn(p, " \t\n.");
            for (k = 0; k < 3; k++)
                if (n == strlen(drv[k]) && strncmp(p, drv[k], n) == 0) {
                    fclose(fp);
                    strncpy(which, drv[k], (size_t) max - 1);
                    which[max - 1] = '\0';
                    return 1;
                }
        }
        fclose(fp);
    }
    return 0;
}

int
vlhe_midi_minor_ok(int minor, char *why, int max)
{
    int unit;

    if (why != NULL && max > 0)
        why[0] = '\0';
    if (minor < 0 || minor > 255) {
        if (why != NULL)
            strncpy(why, "a sound minor is 0 to 255", (size_t) max - 1);
        return 0;
    }
    unit = minor & 15;
    /* unit != 0 FIRST: strchr() finds the terminating NUL for 0, and
     * unit 0 is the mixer chain, the opposite of free. */
    if (unit != 0 && strchr(VLHE_MIDI_MINOR_FREE, unit) != NULL)
        return 1;
    if (unit == 15) {
        char which[32];

        if (!dmfm_driver_present(which, sizeof which))
            return 1;           /* free on this machine - a hand edit */
        if (why != NULL)
            sprintf(why, "minor %d is unit 15, the FM synth node of the"
                         " %.20s driver this machine has", minor, which);
        return 0;
    }
    if (why != NULL) {
        const char *what =
            (unit == 4 || unit == 5)
                ? "an alias of the dsp - a device there can never be opened"
            : (unit == 1 || unit == 6 || unit == 8)
                ? "taken by the sound core, which vmidi needs loaded"
            : "a sound card's own device number";
        sprintf(why, "minor %d is unit %d, %s", minor, unit, what);
    }
    if (why != NULL)
        why[max - 1] = '\0';
    return 0;
}

int
vlhe_set_midiopts(const struct vlhe_midiopts *in)
{
    load_config();

    /* THE RULE IS vlhe_midi_minor_ok()'s - vlhe_backend.h has the table.
     * This used to be "0..255 but not 15", which refused the one free
     * unit a machine might not need refused and accepted the nine that
     * can never work. */
    if (!vlhe_midi_minor_ok(in->minor, NULL, 0))
        return -1;

    {
        struct vlhe_midiopts cur;

        memset(&cur, 0, sizeof cur);
        (void) vlhe_midiopts(&cur);
        sys_set_int("Midi Settings", "Minor", in->minor, cur.minor);
    }
    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

/*
 * PUSH THE SAVED SETTINGS INTO THE RUNNING SYNTH - design/43 6b.
 *
 * WHAT THE "Apply settings" BUTTON CALLS. The settings pages write
 * the config; this makes a running vmidid obey it, without the
 * stop-and-start that drops the channel and silences whatever is
 * playing.
 *
 * THE USER MEASURED WHY IT IS NEEDED, 2026-09-26: playing
 * gmstriving at 22050, changed the setting to 44100, pressed Apply,
 * stopped playmidi, waited past the 3 s release, started again -
 * still 22050. Nothing told the daemon.
 *
 * THE SAVED CONFIG, NOT THE WIDGETS. vlhe_synth() reads what is on
 * disk, so the button means "make the synth match the saved
 * settings" and cannot diverge from what a restart would give.
 *
 * ONE LINE PER SETTING rather than one compound line, because the
 * daemon answers each individually and a partial failure - a rate
 * out of range, say - should not discard the five that were fine.
 *
 * `err' COUNTS RATHER THAN STOPS for the same reason. The caller is
 * told how many did not take; it is not left with a synth half
 * updated and no idea which half.
 *
 * RETURNS the number of settings the daemon REFUSED, or -1 when it
 * could not be reached at all - which the GUI must distinguish,
 * since "not running" and "running and said no" want different
 * messages.
 */
int
vlhe_apply_synth(void)
{
    struct vlhe_synth sy;
    char  req[128], rep[VMIDID_CTL_LINE];
    int   bad = 0;

    if (vlhe_synth(&sy) != 0)
        return -1;

    /* IS IT THERE AT ALL? One cheap request that changes nothing,
     * so a stopped daemon is reported as such rather than as six
     * failed settings. */
    if (vmidi_request("ping", rep, sizeof rep) != 0)
        return -1;

    sprintf(req, "set voices %d", sy.voices);
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    /* AFTER `set voices', which sets the ceiling this restarts from. */
    sprintf(req, "set autovoices %d", sy.auto_voices ? 1 : 0);
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    /* AND ITS SIX SETTINGS, every time, so the daemon matches the
     * file - defaults included, which is how an edit is undone. A file
     * the check refuses counts as one refusal and sends nothing. */
    {
        int  av[VLHE_AUTOVOICE_N];
        char spec[128];

        if (vlhe_autovoice_settings(av, NULL, NULL) != 0) {
            bad++;
        } else {
            vlhe_autovoice_spec(av, spec);
            sprintf(req, "set autovoice %s", spec);
            if (vmidi_request(req, rep, sizeof rep) != 0
                || strncmp(rep, "ok", 2) != 0)
                bad++;
        }
    }

    sprintf(req, "set gain %d", sy.gain_milli);
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    sprintf(req, "set effects %s", VLHE_FX_WORD(sy.reverb, sy.chorus));
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    sprintf(req, "set law %s",
            sy.law == VLHE_LAW_LINEAR ? "linear" : "spec");
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    sprintf(req, "set velfilter %s",
            sy.filter == VLHE_VF_201  ? "201"  :
            sy.filter == VLHE_VF_204  ? "204"  :
            sy.filter == VLHE_VF_NONE ? "none" : "awe");
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    /* THE MODULATION ENVELOPE'S MODE - design/54 D28. Immediate: a
     * switch to reference catches any skipped envelope up at once. */
    sprintf(req, "set modenv %s",
            sy.modenv == VLHE_MODENV_FAST ? "fast" : "reference");
    if (vmidi_request(req, rep, sizeof rep) != 0
        || strncmp(rep, "ok", 2) != 0)
        bad++;

    /*
     * THE RATE LAST, and it is the one that does not take effect
     * now - the daemon defers it to its next release. Sent last so
     * the five immediate ones are already in force when the caller
     * reads the reply and reports "the rate takes effect at the
     * next pause". design/43 6b3: the GUI must say which settings
     * wait, and this is the only one that does.
     */
    if (sy.rate > 0) {
        sprintf(req, "set rate %d", sy.rate);
        if (vmidi_request(req, rep, sizeof rep) != 0
            || strncmp(rep, "ok", 2) != 0)
            bad++;
    }

    return bad;
}


/* ------------------------------------------------------------------ */
/* Preferences - the interface font                                   */
/* ------------------------------------------------------------------ */

int
vlhe_prefs(struct vlhe_prefs *out)
{
    load_config();
    memset(out, 0, sizeof *out);

    /* "" MEANS LEAVE GTK ALONE - the absence of an override, not
     * "Helvetica 12" written out. A user who later sets a real gtkrc
     * then gets what they asked for. */
    strncpy(out->font,
            vlhe_conf_get(&cfg_usr, "Interface", "Font", ""),
            sizeof out->font - 1);
    out->font[sizeof out->font - 1] = '\0';
    /* 0 = Status. Out of range is corrected by the caller, which is
     * the only place that knows how many modules there are. */
    out->start_page = vlhe_conf_get_int(&cfg_usr, "Interface",
                                        "StartPage", 0);

    /*
     * -1 WHEN THE KEY IS ABSENT OR EMPTY, which is what lets the
     * default differ by mode without an explicit 0 being mistaken for
     * "never set". vlhe_show_apply_result() resolves it.
     */
    {
        const char *v = vlhe_conf_get(&cfg_usr, "Interface",
                                      "ShowApplyResult", "");

        if (v[0] == '\0')
            out->show_apply_result = -1;
        else
            out->show_apply_result = atoi(v) != 0;
    }
    return 0;
}

int
vlhe_show_apply_result(void)
{
    struct vlhe_prefs p;

    if (vlhe_prefs(&p) != 0)
        return vlhe_self_is_trial();    /* no config: the mode decides */

    if (p.show_apply_result >= 0)
        return p.show_apply_result;

    /*
     * THE MODE'S DEFAULT. Portable on, installed off - see the
     * struct's comment for why, and note this is the one place that
     * decides, so the GUI never has to know the rule.
     */
    return vlhe_self_is_trial();
}

int
vlhe_set_prefs(const struct vlhe_prefs *in)
{
    load_config();
    vlhe_conf_set(&cfg_usr, "Interface", "Font", in->font);
    vlhe_conf_set_int(&cfg_usr, "Interface", "StartPage", in->start_page);

    /* -1 CLEARS IT, so "use the mode's default" is expressible from
     * the dialog and not only by hand-editing the file. */
    if (in->show_apply_result < 0)
        vlhe_conf_set(&cfg_usr, "Interface", "ShowApplyResult", "");
    else
        vlhe_conf_set_int(&cfg_usr, "Interface", "ShowApplyResult",
                          in->show_apply_result ? 1 : 0);

    /*
     * MEMORY ONLY - vlhe_commit() writes. Changed 2026-09-29, and
     * this function used to be the exception.
     *
     * IT WROTE THE USER CONFIG IMMEDIATELY, on this reasoning: "this
     * is not a form field behind Apply. It is an action the user took
     * in a dialog that has already closed, so there is no later
     * commit point to carry it." The premise was that a font change
     * could not take effect until the next start, so deferring the
     * write would have deferred the setting itself.
     *
     * THE FONT IS LIVE NOW (`vlhe_apply_font()' in vlhe_cc.c), so
     * there IS a later commit point - the same Apply every other page
     * uses - and the exception has no reason left.
     *
     * AND IT CLOSES design/36 ROW 93. That write was the third and
     * last of the unconditional writers; the other two were fixed
     * 2026-09-27. In PORTABLE mode with no user config it created
     * one, which flipped `vlhe_conf_exists()' true, which correctly
     * hid the sidebar's "no configuration file" notice - so the GUI
     * stopped saying the one true thing about its own state. The row:
     * "Nothing about the sidebar is broken; it is reporting a state
     * that the unwanted write changed."
     *
     * NO `cfg_usr.dirty = 0' EITHER, and that is the point rather
     * than an omission: the keys above raised it and it must STAY
     * raised, because the settings really are unsaved until Apply.
     * The line that cleared it existed only because this function was
     * its own commit point.
     */
    return 0;
}

int
vlhe_font_families(char out[][VLHE_NAME_MAX], int max)
{
    /*
     * WHAT THIS MACHINE'S X SERVER HAS, read rather than guessed -
     * "a list of fonts that are not installed is worse than no list".
     *
     * NOT READ FROM THE SERVER HERE, and that is deliberate: asking X
     * means XListFonts, which means the backend linking Xlib and
     * knowing about a display connection. vlhe_backend.h's first rule
     * is that no GTK type appears in it, and a Display is the same
     * category of thing.
     *
     * SO THE MODULE THAT OWNS THE DIALOG ASKS, and this returns the
     * target's own shipped set as a fallback for a caller that cannot
     * (CLAUDE.md: helvetica, times, courier, lucida, charter).
     */
    static const char *const fams[] = {
        "helvetica", "times", "courier", "lucida", "charter"
    };
    int i, n = 0;

    for (i = 0; i < (int)(sizeof fams / sizeof fams[0]) && n < max; i++) {
        strncpy(out[n], fams[i], VLHE_NAME_MAX - 1);
        out[n][VLHE_NAME_MAX - 1] = '\0';
        n++;
    }
    return n;
}

int
vlhe_font_sizes(int *out, int max)
{
    static const int sizes[] = { 8, 10, 12, 14, 18, 24 };
    int i, n = 0;

    for (i = 0; i < (int)(sizeof sizes / sizeof sizes[0]) && n < max; i++)
        out[n++] = sizes[i];
    return n;
}

/* ------------------------------------------------------------------ */
/* Overall status, the mixer, and lifecycle                           */
/* ------------------------------------------------------------------ */

int
vlhe_status(struct vlhe_status *out)
{
    struct vlhe_sound snd;

    memset(out, 0, sizeof *out);
    out->vsound_loaded = vlhe_status_module_loaded("vsound");
    out->vmidi_loaded  = vlhe_status_module_loaded("vmidi");
    out->vdisc_loaded  = vlhe_status_module_loaded("vdisc");
    out->vsoundd_running =
        vlhe_status_daemon(vlhe_status_pidfile_path("vsoundd", -1)) > 0;
    out->vdiscd_running =
        vlhe_status_daemon(vlhe_status_pidfile_path("vdiscd", -1)) > 0;

    vlhe_sound(&snd);
    strncpy(out->card, snd.card, sizeof out->card - 1);
    out->card[sizeof out->card - 1] = '\0';
    return 0;
}

int
vlhe_sound_present(void)
{
    /*
     * THE GUI MUST NOT BLOCK AN UNLOAD - design/09, and the reason
     * this is a function rather than a cached flag. A user CAN rmmod
     * from a terminal while the window sits there, so the backend
     * reopens per operation and never holds a handle. This says
     * whether the last attempt found anything.
     */
    struct vsound_chanlist cl;

    /* /proc/modules FIRST, as chanlist() does since design/47 B3: the
     * Volume page asks this every 250 ms now (its poll tracks presence,
     * 2026-10-01), and opening a stale /dev/dsp link with vsound gone
     * is what forked `modprobe sound-slot-N' eight times a second on
     * 86Box (tests/logs/2026-09-30-86box-guest-var-log-kern/). */
    /*
     * AND NO OPEN OF ITS OWN SINCE 2026-10-02: it asks the channel
     * list, which the same poll is about to ask anyway (chanlist()
     * keeps the answer 100 ms), so "is it there" costs no second open
     * - and "answers VSOUND_IOC_CHANS" is a better test of "vsound is
     * there" than "the node opens", which a real card's node passes.
     */
    if (!vlhe_status_module_loaded("vsound"))
        return 0;
    return chanlist(&cl) == 0;
}

int
vlhe_mixer_channels(struct vlhe_mixer_chan *out, int max)
{
    return vlhe_mixer_read(out, max);
}

int
vlhe_mixer_restore_enabled(void)
{
    load_config();
    return vlhe_conf_get_int(&cfg_sys, "Sound Settings",
                             "SaveMixerLevels", 0);
}

/*
 * THE INTERFACE SETTINGS, AND THEY LIVE IN THE USER'S FILE.
 *
 * VerticalSliders is VLHE_TPL_USER in the template and had NO SETTER
 * AT ALL until 2026-09-19 - volume_collect() called
 * volume_set_vertical(), which moves a static and redraws, and
 * nothing ever reached cfg_usr. So the checkbox applied for the
 * session and was gone at the next start, and because cfg_usr was
 * never marked dirty the user's file was never written either.
 *
 * THAT WAS THE WHOLE "only the main config gets written" SYMPTOM:
 * SaveMixerLevels goes to cfg_sys and did persist, so Apply looked
 * like it worked while every USER setting was discarded.
 *
 * AND `Recent Images' WAS NOT THE EXCEPTION THIS NOTE ONCE CLAIMED.
 * It said nothing outside that section had ever put a value into
 * cfg_usr, which reads as though the recent list worked. It did not:
 * only the REMOVE path wrote, so the section could be emptied and
 * never filled. See recent_touch() above - added 2026-09-19, after
 * the user found the menu permanently `(no recent images)'.
 *
 * MEMORY ONLY, like every other setter here - vlhe_commit() writes.
 */
/*
 * THE `.old' BACKUP - IS THERE ONE, WHEN WAS IT MADE, AND PUT IT BACK.
 *
 * vlhe_conf_template_write() has always written <path>.old before
 * replacing a file, and until 2026-09-19 NOTHING EVER READ IT BACK -
 * insurance nobody could claim (the user found this on 86Box).
 *
 * ONLY THE SYSTEM FILE HAS ONE. vlhe_commit() passes backup=1 for
 * /etc/vlhe.conf and 0 for the user file, deliberately: the user file
 * holds levels and a recent list, where a stale `.old' is clutter
 * rather than insurance (design/33 section 3). So everything here is
 * about the system file alone.
 */
int
vlhe_conf_backup_exists(void)
{
    char        path[VLHE_PATH_MAX];
    const char *p = cfg_sys_path;
    struct stat st;

    if (p == NULL || strlen(p) + 5 >= sizeof path)
        return 0;
    sprintf(path, "%s.old", p);
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

/* WHEN, as a date the dialog can show. A backup with no date is hard
 * to trust - "restore it" is a different decision for this afternoon
 * than for last year. Returns 0 and leaves buf empty if unknown. */
int
vlhe_conf_backup_when(char *buf, size_t len)
{
    char         path[VLHE_PATH_MAX];
    const char  *p = cfg_sys_path;
    struct stat  st;
    struct tm   *tm;

    if (buf == NULL || len == 0)
        return 0;
    buf[0] = '\0';

    if (p == NULL || strlen(p) + 5 >= sizeof path)
        return 0;
    sprintf(path, "%s.old", p);
    if (stat(path, &st) != 0)
        return 0;

    tm = localtime(&st.st_mtime);
    if (tm == NULL)
        return 0;

    /* strftime, not sprintf on the struct fields - it is C89 and it
     * respects the locale's idea of a date. */
    return strftime(buf, len, "%Y-%m-%d %H:%M", tm) > 0 ? 1 : 0;
}

/*
 * PUT IT BACK: copy <sys>.old over <sys>, then reload.
 *
 * COPY RATHER THAN RENAME, so the backup SURVIVES the restore. A
 * rename would leave the user with a restored config and no backup,
 * and the next save would immediately overwrite the only copy of
 * whatever they just recovered from.
 *
 * The reload afterwards is what makes the restored settings live:
 * cfg_loaded is cleared so load_config() re-reads both files, exactly
 * as it would at startup.
 */
int
vlhe_conf_restore_backup(struct vlhe_commit_err *err)
{
    char        path[VLHE_PATH_MAX];
    const char *p = cfg_sys_path;

    if (err != NULL) {
        err->path[0] = '\0';
        err->why[0]  = '\0';
    }

    if (p == NULL || strlen(p) + 5 >= sizeof path) {
        commit_fail(err, p, "path too long");
        return -1;
    }
    sprintf(path, "%s.old", p);

    if (!vlhe_conf_backup_exists()) {
        commit_fail(err, path, "no backup to restore");
        return -1;
    }

    /*
     * ONLY A BACKUP ROOT COULD HAVE WRITTEN - design/55 recommendation 2
     * (section 6 row 3, R3), 2026-10-04. Restored by root, a .old a user
     * made would become a root-owned live file that then PASSES the
     * trust check every later read applies (design/49 T0). So, as root,
     * the backup is held to the same test as the system file itself.
     * Static: the struct is large and this is not re-entered.
     */
    if (geteuid() == 0) {
        static struct vlhe_conf probe;
        char why[200];

        if (vlhe_conf_read_trusted(&probe, path, why, (int) sizeof why) != 0) {
            commit_fail(err, path, why[0] != '\0' ? why
                        : "the backup cannot be judged");
            return -1;
        }
    }

    if (vlhe_conf_copy(path, p) != 0) {
        commit_fail(err, p, strerror(errno));
        return -1;
    }

    /* RE-READ. Without this the program keeps the defaults it started
     * with and the restored file only takes effect at the next run -
     * which would look like the restore had failed. */
    cfg_loaded = 0;
    cfg_sys.dirty = 0;
    cfg_usr.dirty = 0;
    load_config();
    return 0;
}

int
vlhe_ui_vertical_sliders(void)
{
    load_config();
    return vlhe_conf_get_int(&cfg_usr, "Interface", "VerticalSliders", 0);
}

/* THE CD+G VIEWER'S OPTIONS - vlhe_backend.h has the fields. */
int
vlhe_cdg_prefs(struct vlhe_cdg_prefs *out)
{
    const char *v;

    load_config();
    memset(out, 0, sizeof *out);
    out->zoom = vlhe_conf_get_int(&cfg_usr, "CDG Viewer", "Zoom", 2);
    out->anchor = vlhe_conf_get_int(&cfg_usr, "CDG Viewer", "Anchor", 5);
    out->track_format = vlhe_conf_get_int(&cfg_usr, "CDG Viewer",
                                          "TrackFormat", 0);
    out->border_mask = vlhe_conf_get_int(&cfg_usr, "CDG Viewer",
                                         "BorderMask", 1);
    out->clear_on_eject = vlhe_conf_get_int(&cfg_usr, "CDG Viewer",
                                            "ClearOnEject", 1);
    out->compact_detached = vlhe_conf_get_int(&cfg_usr, "CDG Viewer",
                                              "CompactDetached", 0);
    v = vlhe_conf_get(&cfg_usr, "CDG Viewer", "InfoFields",
                      "performer,title");
    strncpy(out->info_fields, v, sizeof out->info_fields - 1);
    out->info_fields[sizeof out->info_fields - 1] = '\0';
    /* A DAMAGED VALUE READS AS THE DEFAULT, not as nonsense the page
     * then draws with - the same rule vlhe_mixer_restore() follows. */
    if (out->zoom != 1 && out->zoom != 2)
        out->zoom = 2;
    if (out->anchor < 1 || out->anchor > 9)
        out->anchor = 5;
    if (out->track_format != 0 && out->track_format != 1)
        out->track_format = 0;
    if (out->border_mask != 0 && out->border_mask != 1)
        out->border_mask = 1;
    if (out->clear_on_eject != 0 && out->clear_on_eject != 1)
        out->clear_on_eject = 1;
    if (out->compact_detached != 0 && out->compact_detached != 1)
        out->compact_detached = 0;
    return 0;
}

/* Is `fields' a comma list drawn from the eight names the info line
 * knows? Empty is allowed - a line showing nothing. */
static int
cdg_fields_ok(const char *fields)
{
    static const char *known[] = { "title", "performer", "songwriter",
                                   "composer", "arranger", "message",
                                   "track", "time" };
    char buf[VLHE_CDG_FIELDS_MAX];
    char *tok;

    if (strlen(fields) >= sizeof buf)
        return 0;
    strcpy(buf, fields);
    for (tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        int k, hit = 0;

        for (k = 0; k < 8 && !hit; k++)
            if (strcmp(tok, known[k]) == 0)
                hit = 1;
        if (!hit)
            return 0;
    }
    return 1;
}

int
vlhe_set_cdg_prefs(const struct vlhe_cdg_prefs *in)
{
    if (in == NULL)
        return -1;
    if ((in->zoom != 1 && in->zoom != 2)
        || in->anchor < 1 || in->anchor > 9
        || (in->track_format != 0 && in->track_format != 1)
        || (in->border_mask != 0 && in->border_mask != 1)
        || (in->clear_on_eject != 0 && in->clear_on_eject != 1)
        || (in->compact_detached != 0 && in->compact_detached != 1)
        || !cdg_fields_ok(in->info_fields))
        return -1;
    load_config();
    /* vlhe_conf_set*() compare before they dirty, so an OK that changed
     * nothing leaves the file clean - design/49 T0 fix 3's rule. */
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "Zoom", in->zoom);
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "Anchor", in->anchor);
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "TrackFormat", in->track_format);
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "BorderMask", in->border_mask);
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "ClearOnEject",
                      in->clear_on_eject);
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "CompactDetached",
                      in->compact_detached);
    vlhe_conf_set(&cfg_usr, "CDG Viewer", "InfoFields", in->info_fields);
    return 0;
}

int
vlhe_cdg_drive(void)
{
    int d;

    load_config();
    d = vlhe_conf_get_int(&cfg_usr, "CDG Viewer", "Drive", 0);
    /* A DAMAGED VALUE READS AS THE DEFAULT, as vlhe_cdg_prefs() does. */
    return (d < 0 || d >= VLHE_MAX_DRIVE) ? 0 : d;
}

int
vlhe_set_cdg_drive(int index)
{
    if (index < 0 || index >= VLHE_MAX_DRIVE)
        return -1;
    load_config();
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", "Drive", index);
    /* WHICH DISC THE VIEWER SEES CHANGES - forget the drive rows so
     * its next look is a fresh one (the user, 2026-10-03). */
    vlhe_drives_forget();
    return 0;
}

/* The two viewer rules - a 0/1 key in [CDG Viewer]; a damaged value
 * reads as the default. */
static int
cdg_flag(const char *key, int dflt)
{
    int v;

    load_config();
    v = vlhe_conf_get_int(&cfg_usr, "CDG Viewer", key, dflt);
    return (v == 0 || v == 1) ? v : dflt;
}

static int
cdg_set_flag(const char *key, int on)
{
    if (on != 0 && on != 1)
        return -1;
    load_config();
    vlhe_conf_set_int(&cfg_usr, "CDG Viewer", key, on);
    vlhe_drives_forget();       /* as vlhe_set_cdg_drive() */
    return 0;
}

int vlhe_cdg_follow_any(void)          { return cdg_flag("FollowAnyDisc", 1); }
int vlhe_set_cdg_follow_any(int on)    { return cdg_set_flag("FollowAnyDisc", on); }
int vlhe_cdg_stop_on_swap(void)        { return cdg_flag("StopOnSwap", 0); }
int vlhe_set_cdg_stop_on_swap(int on)  { return cdg_set_flag("StopOnSwap", on); }

int
vlhe_ui_set_vertical_sliders(int on)
{
    load_config();
    vlhe_conf_set_int(&cfg_usr, "Interface", "VerticalSliders", on ? 1 : 0);
    return 0;
}

int
vlhe_mixer_set_restore(int on)
{
    if (on != 0 && on != 1)
        return -1;              /* a toggle - design/49 T0 */
    load_config();
    sys_set_int("Sound Settings", "SaveMixerLevels", on ? 1 : 0,
                vlhe_mixer_restore_enabled() ? 1 : 0);
    /* MEMORY ONLY - vlhe_commit() writes. */
    return 0;
}

/*
 * PER-PROGRAM LEVELS ACROSS A RELOAD OR A REBOOT - design/33 section
 * 1c, built 2026-10-02. The module remembers each program's level by
 * name for as long as it is loaded (vsound.h, VSOUND_IOC_PROGGET);
 * these keep that table in a file, per machine like the mixers, when
 * Save Program Levels is set: saved by the unload plan's first steps,
 * pushed back by the load plan straight after vsound is in.
 *
 * THE FILE: one [Program Levels] section, a key per program name,
 * and `40' or `40 muted' - oldest first, so pushing it back keeps the
 * order in which a full table gives names up.
 */
int
vlhe_progvol_enabled(void)
{
    load_config();
    return vlhe_conf_get_int(&cfg_sys, "Sound Settings",
                             "SaveProgramLevels", 0) ? 1 : 0;
}

int
vlhe_progvol_set_enabled(int on)
{
    if (on != 0 && on != 1)
        return -1;
    load_config();
    sys_set_int("Sound Settings", "SaveProgramLevels", on,
                vlhe_progvol_enabled());
    return 0;
}

const char *
vlhe_progvol_state_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *env = getenv("VLHE_VOLUMES");

    if (env != NULL && *env != '\0') {
        strncpy(buf, env, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        return buf;
    }
    /* BESIDE A PORTABLE FOLDER, as the mixers and the session are. */
    if (vlhe_self_is_trial()
        && vlhe_self_path("volumes", buf, sizeof buf))
        return buf;
    strcpy(buf, "/var/lib/vlhe/volumes");
    return buf;
}

/* A NAME THE FILE CAN HOLD AS A KEY: printable, no `=', not starting
 * like a comment or a section, no space at either end (the reader
 * trims). A program called something else is simply not kept. */
static int
progvol_name_ok(const char *s)
{
    int i, n = (int) strlen(s);

    if (n == 0 || s[0] == ';' || s[0] == '#' || s[0] == '['
        || s[0] == ' ' || s[n - 1] == ' ')
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] > 0x7e || s[i] == '=')
            return 0;
    return 1;
}

/* THE TABLE TO THE FILE'S SECTION AND BACK - split out so the host
 * test can check the format with no vsound to ask (vlhe_progvol.h). */
int
vlhe_progvol_to_conf(const struct vsound_proglist *pl, struct vlhe_conf *c)
{
    char name[VSOUND_COMM_LEN], val[32];
    int i, kept = 0;

    for (i = 0; i < (int) pl->nprog && i < VSOUND_PROG_MAX; i++) {
        memcpy(name, pl->prog[i].comm, VSOUND_COMM_LEN);
        name[VSOUND_COMM_LEN - 1] = '\0';
        if (!progvol_name_ok(name))
            continue;
        sprintf(val, "%u%s", (unsigned) pl->prog[i].vol,
                (pl->prog[i].flags & VSOUND_PROG_MUTED) ? " muted" : "");
        vlhe_conf_set(c, "Program Levels", name, val);
        kept++;
    }
    return kept;
}

int
vlhe_progvol_from_conf(const struct vlhe_conf *c, struct vsound_proglist *pl)
{
    int i, n = 0;

    memset(pl, 0, sizeof *pl);
    for (i = 0; i < c->n && n < VSOUND_PROG_MAX; i++) {
        const char *v = c->entry[i].value;
        char *end;
        long vol;

        if (strcmp(c->entry[i].section, "Program Levels") != 0
            || !progvol_name_ok(c->entry[i].key)
            || strlen(c->entry[i].key) >= VSOUND_COMM_LEN)
            continue;
        vol = strtol(v, &end, 10);
        if (end == v || vol < 0 || vol > VLHE_VOL_BOOST)
            continue;               /* a hand-edited value we cannot read */
        while (*end == ' ')
            end++;
        if (*end != '\0' && strcmp(end, "muted") != 0)
            continue;               /* `40 loud' - not ours to guess */
        strcpy(pl->prog[n].comm, c->entry[i].key);
        pl->prog[n].vol = (__u32) vol;
        pl->prog[n].flags = *end != '\0' ? VSOUND_PROG_MUTED : 0;
        n++;
    }
    pl->nprog = (__u32) n;
    return n;
}

int
vlhe_progvol_save_all(const char *path)
{
    static struct vlhe_conf c;          /* large: not on the stack */
    struct vsound_proglist pl;
    int fd, kept;

    if (path == NULL)
        path = vlhe_progvol_state_path();
    /* NOT WITHOUT vsound - design/55 R11, design/54 D53 (2026-10-04). vsound_device() answers
     * a literal /dev/dsp when vsound is not loaded, and an unload after a
     * half-failed load reached here: opening the card's node, or an empty
     * minor that an alias would load a driver for. No vsound, no table. */
    if (!vlhe_status_module_loaded("vsound"))
        return -1;
    fd = open(vsound_device(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    memset(&pl, 0, sizeof pl);
    if (ioctl(fd, VSOUND_IOC_PROGGET, &pl) < 0) {
        close(fd);
        return -1;                      /* an older module: no table */
    }
    close(fd);

    memset(&c, 0, sizeof c);
    kept = vlhe_progvol_to_conf(&pl, &c);

    /* WRITTEN EVEN WHEN EMPTY, unlike the mixers: a user who put
     * every program back to 100% must not have the old levels come
     * back at the next load from a file nobody rewrote. */
    (void) vlhe_conf_mkdir_for(path);
    if (vlhe_conf_write(&c, path,
            "Each program's level, saved by VLHE at unload and pushed back\n"
            "into vsound at load when Save Program Levels is set. A key per\n"
            "program name; `40' or `40 muted'. Rewritten at every unload.",
            0) != 0)
        return -1;
    return kept;
}

int
vlhe_progvol_restore_all(const char *path)
{
    static struct vlhe_conf c;
    struct vsound_proglist pl;
    struct stat sb;
    int fd, n;

    if (path == NULL)
        path = vlhe_progvol_state_path();
    if (stat(path, &sb) != 0)
        return 0;                       /* never saved: nothing to do */
    memset(&c, 0, sizeof c);
    if (vlhe_conf_read(&c, path) < 0)
        return -1;

    n = vlhe_progvol_from_conf(&c, &pl);

    /* NOT WITHOUT vsound - design/55 R11, design/54 D53 (2026-10-04). vsound_device() answers
     * a literal /dev/dsp when vsound is not loaded, and an unload after a
     * half-failed load reached here: opening the card's node, or an empty
     * minor that an alias would load a driver for. No vsound, no table. */
    if (!vlhe_status_module_loaded("vsound"))
        return -1;
    fd = open(vsound_device(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (ioctl(fd, VSOUND_IOC_PROGSET, &pl) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return n;
}

int
vlhe_backend_init(void)
{
    /* RETURNS 0 ALWAYS, which the header specifies: "a backend that
     * cannot reach anything is not an error, it is an empty machine". */
    load_config();
    return 0;
}

void
vlhe_backend_fini(void)
{
    /* Nothing is held open - see vlhe_sound_present(). */
}

const char *
vlhe_backend_name(void)
{
    return "real";
}
