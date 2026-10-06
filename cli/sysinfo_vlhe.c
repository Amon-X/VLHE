/*
 * sysinfo_vlhe.c - sysinfo-vlhe, the report a user runs and sends us:
 * this machine, this copy of VLHE, and what the backend sees.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * RENAMED 2026-10-02 from vlhe-probe (vlhe_probe.c) by the user, with
 * its report file to match - sysinfo-vlhe-<date>.txt - and moved to
 * tools/ (installed: $prefix/share/vlhe/tools/), off the PATH.
 *
 * (ORIGINALLY vlhe-probe - what the backend sees on a real machine.)
 *
 * WHY THIS EXISTS. Five backend pieces were written and host-tested
 * against files the tests themselves wrote; none had met a real
 * /proc/modules, a real /dev/mixer or a real ext2 filesystem. A GUI
 * failure would say something is wrong; this says WHAT.
 *
 * It PRINTS AND DOES NOT DECIDE. Every value is reported as found, so
 * a wrong assumption shows up as an odd number rather than as a crash
 * or a silently empty list. Read the output beside the host tests'
 * expectations - where they disagree, the machine is right.
 *
 * WHAT IT WRITES: the report, as a file in the current directory (or
 * /tmp if that cannot be written) as well as to the screen, and one
 * scratch config under /tmp that exercises the config writer. It does
 * NOT touch /etc, /var/lib or the mixer's levels - it reads those.
 *
 * 2026-10-02 - BECOMING THE REPORT A USER SENDS US, the user's call:
 * (1) `-w', which wrote the real /etc/vlhe.conf, is GONE - a report
 * tool must never change the configuration it reports on; (2) the
 * report goes to a file to send; (3) the kernel and MODVERSIONS, the
 * VLHE version and stamp and portable-or-installed, the /dev/dsp and
 * /dev/cdrom redirects, the session file and DAEMON.LOG's tail, added;
 * (4) /proc/sound is NEVER read - see probe_devices(); (5) the report
 * opens by saying what is in it. Its new name and its place in tools/
 * and its place in tools/ followed the same evening (above).
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>

#include "vlhe_conf.h"
#include "vlhe_conf_template.h"
#include "vlhe_fontscan.h"
#include "vlhe_mixer.h"
#include "vlhe_status.h"
#include "vlhe_self.h"
#include "vlhe_session.h"
#include "vlhe_journal.h"

#ifndef VLHE_VERSION
#define VLHE_VERSION "unknown"
#endif

static void
rule(const char *title)
{
    printf("\n=== %s ", title);
    {
        int n = 66 - (int)strlen(title);
        while (n-- > 0)
            putchar('=');
    }
    putchar('\n');
}

/* ------------------------------------------------------------------ */

static void cat_file(const char *path, const char *why, int max_lines);

/* Dump the LAST `keep' lines of a file - for a log, where the end is
 * what matters and the whole file may be long. */
static void
tail_file(const char *path, const char *why, int keep)
{
    static char ring[64][256];
    FILE *fp;
    int  n = 0, i, start;

    if (keep > 64)
        keep = 64;
    printf("\n--- %s (last %d lines)\n", path, keep);
    if (why != NULL)
        printf("    (%s)\n", why);
    fp = fopen(path, "r");
    if (fp == NULL) {
        printf("    ABSENT: %s\n", strerror(errno));
        return;
    }
    while (fgets(ring[n % keep], sizeof ring[0], fp) != NULL)
        n++;
    fclose(fp);
    if (n == 0) {
        printf("    (empty)\n");
        return;
    }
    start = n > keep ? n - keep : 0;
    for (i = start; i < n; i++) {
        const char *l = ring[i % keep];

        printf("    %s", l);
        if (l[0] == '\0' || l[strlen(l) - 1] != '\n')
            putchar('\n');
    }
}

/*
 * THE MACHINE AND THIS COPY OF VLHE - what we ask first in every bug
 * report: which kernel, built which way, and which build of ours.
 *
 * MODVERSIONS IS READ FROM /proc/ksyms, not from a source tree a user
 * may not have: a versioned kernel exports `printk_R1b7d4074', an
 * unversioned one `printk'. The two are incompatible at the symbol
 * level (CLAUDE.md section 3), so a module built the other way fails
 * at insmod with a CRC mismatch that reads like corruption.
 */
static void
probe_system(void)
{
    struct utsname u;
    char  line[256];
    FILE *fp;
    int   mv = -1;

    rule("THIS MACHINE AND THIS COPY OF VLHE");

    if (uname(&u) == 0)
        printf("kernel:      %s %s %s %s\n", u.sysname, u.release,
               u.version, u.machine);
    fp = fopen("/proc/version", "r");
    if (fp != NULL) {
        if (fgets(line, sizeof line, fp) != NULL)
            printf("/proc/version: %s", line);
        fclose(fp);
    }

    fp = fopen("/proc/ksyms", "r");
    if (fp != NULL) {
        while (mv < 0 && fgets(line, sizeof line, fp) != NULL) {
            char addr[32], name[200];

            if (sscanf(line, "%31s %199s", addr, name) != 2)
                continue;
            if (strcmp(name, "printk") == 0)
                mv = 0;
            else if (strncmp(name, "printk_R", 8) == 0)
                mv = 1;
        }
        fclose(fp);
    }
    printf("MODVERSIONS: %s\n",
           mv == 1 ? "ON  (symbols carry _R checksums - /proc/ksyms)"
           : mv == 0 ? "OFF (plain symbol names - /proc/ksyms)"
           : "unknown (printk not found in /proc/ksyms)");

    printf("\nVLHE:        version %s, this report built " __DATE__
           " " __TIME__ "\n", VLHE_VERSION);
    printf("running from: %s\n",
           vlhe_self_dir() != NULL ? vlhe_self_dir() : "(unknown)");
    if (vlhe_self_is_trial()) {
        char stamp[VLHE_PATH_MAX];

        printf("mode:        PORTABLE (built with make portable)\n");
        if (vlhe_self_path("STAMP", stamp, sizeof stamp))
            cat_file(stamp, "which build this folder is", 4);
    } else {
        printf("mode:        INSTALLED (built with make)\n");
    }
}

/*
 * THE DEVICE NODES VLHE REDIRECTS - /dev/dsp and /dev/cdrom are made
 * links to ours while VLHE is loaded and put back on unload, and most
 * "no sound after X" reports are one of those left pointing at the
 * wrong thing. lstat(), so a link is shown as a link.
 *
 * /proc/sound IS NEVER READ, HERE OR ANYWHERE IN THIS PROGRAM. On 2.2
 * it OOPSES the kernel on a machine with more than five audio devices
 * and a legacy card (CLAUDE.md section 5) - a diagnostic tool must not
 * be the thing that crashes the machine it is diagnosing.
 */
static void
probe_devices(void)
{
    static const char *const nodes[] = {
        "/dev/dsp", "/dev/dsp0", "/dev/dsp1", "/dev/dsp2", "/dev/dsp3",
        "/dev/dsp4", "/dev/dsp5", "/dev/dsp6", "/dev/dsp7",
        "/dev/mixer", "/dev/cdrom", "/dev/vmidi"
    };
    int i;

    rule("DEVICE NODES");

    for (i = 0; i < (int)(sizeof nodes / sizeof nodes[0]); i++) {
        struct stat sb;
        char tgt[256];
        int  n;

        printf("  %-12s ", nodes[i]);
        if (lstat(nodes[i], &sb) != 0) {
            printf("absent\n");
        } else if (S_ISLNK(sb.st_mode)) {
            n = (int) readlink(nodes[i], tgt, sizeof tgt - 1);
            tgt[n > 0 ? n : 0] = '\0';
            printf("link -> %s\n", tgt);
        } else if (S_ISCHR(sb.st_mode) || S_ISBLK(sb.st_mode)) {
            printf("%s %d,%d\n", S_ISCHR(sb.st_mode) ? "char " : "block",
                   (int) major(sb.st_rdev), (int) minor(sb.st_rdev));
        } else {
            printf("not a device node\n");
        }
    }
}

/*
 * WHAT THE LAST LOAD CHANGED AND WHAT THE DAEMONS SAID - the session
 * file (what an unload will put back) and the end of DAEMON.LOG. Both
 * found the way the programs find them, portable or installed - and an
 * installed copy follows a portable load's pointer (design/40 sec. 9).
 */
static void
probe_session(void)
{
    rule("THE LAST LOAD");
    cat_file(vlhe_session_path(),
             "the session file - what the unload will put back", 80);
    tail_file(vlhe_daemon_log_path(), "what the daemons said", 40);
}

static void
probe_modules(void)
{
    static const char *const mods[] = {
        "vsound", "vdisc", "vmidi",             /* ours */
        "soundcore", "sound", "midi",           /* the OSS core */
        "sb", "esssolo1", "es1371", "emu10k1",  /* cards we test on */
        "opl3", "uart401"
    };
    int i;

    rule("MODULES");
    printf("reading: %s\n\n", vlhe_status_modules_path());

    for (i = 0; i < (int)(sizeof mods / sizeof mods[0]); i++) {
        int loaded = vlhe_status_module_loaded(mods[i]);
        int users  = vlhe_status_module_users(mods[i]);

        printf("  %-12s %-10s", mods[i], loaded ? "LOADED" : "-");
        if (loaded) {
            if (users >= 0)
                printf("used by %d", users);
            else
                printf("use count unreadable");
        }
        putchar('\n');
    }

    /* WHAT THE PARSER ACTUALLY SAW. If a name above reads "-" and you
     * know it is loaded, the first field of its line is not what we
     * expect - and this is the evidence for that. */
    {
        FILE *fp = fopen(vlhe_status_modules_path(), "r");
        char line[256];
        int n = 0;

        printf("\n  the file itself:\n");
        if (fp == NULL) {
            printf("    (cannot open: %s)\n", strerror(errno));
            return;
        }
        while (fgets(line, sizeof line, fp) != NULL && n < 24) {
            printf("    %s", line);
            n++;
        }
        fclose(fp);
        if (n == 0)
            printf("    (empty)\n");
    }
}

/* Dump a file verbatim, or say why not. For the two files that decide
 * what loads at boot - which no other tool here captures, and which a
 * SCREENSHOT cannot be grepped for later (CLAUDE.md's rule about
 * transcribing what an image shows). */
static void
cat_file(const char *path, const char *why, int max_lines)
{
    FILE *fp;
    char line[512];
    int n = 0;

    printf("\n--- %s\n", path);
    if (why != NULL)
        printf("    (%s)\n", why);

    fp = fopen(path, "r");
    if (fp == NULL) {
        printf("    ABSENT: %s\n", strerror(errno));
        return;
    }
    while (fgets(line, sizeof line, fp) != NULL && n < max_lines) {
        printf("    %s", line);
        if (line[strlen(line) - 1] != '\n')
            putchar('\n');
        n++;
    }
    if (n == 0)
        printf("    (empty)\n");
    fclose(fp);
}

/* WHAT LOADS AT BOOT, AND WHAT COREL THINKS THE HARDWARE IS.
 *
 * /proc/modules above says what is loaded NOW. These say what the
 * machine will load next time and why - which is a different question
 * and the one that matters for installing anything.
 *
 * AND /etc/modules IS REWRITTEN BY THE SYSTEM ITSELF.
 * corel-etc/devices.d/{audio,network,usb-stack,video,winmodem} each
 * remove their driver's line and append it again, from
 * S80devicesupdate.sh, whenever detected hardware changes. So a line
 * put here by hand or by an installer is not guaranteed to stay where
 * it was put - see design/09's load-order section. Capturing the file
 * before and after a hardware change is how that gets settled. */
static void
probe_boot_config(void)
{
    rule("WHAT LOADS AT BOOT");

    cat_file("/etc/modules", "loaded at boot, in this order", 40);
    cat_file("/etc/conf.modules",
             "GENERATED by update-modules from /etc/modutils", 60);
    cat_file("/etc/devices",
             "what Corel's own detection thinks is installed", 80);
}

static void
probe_daemons(void)
{
    static const char *const daemons[] = { "vsoundd", "vmidid", "vdiscd" };
    int i;

    rule("DAEMONS");
    printf("pid files in: %s\n\n", vlhe_status_rundir());

    for (i = 0; i < 3; i++) {
        const char *path = vlhe_status_pidfile_path(daemons[i], -1);
        int st = vlhe_status_daemon(path);

        printf("  %-10s %-34s ", daemons[i], path);
        if (st > 0)
            printf("RUNNING, pid %d\n", st);
        else if (st == 0)
            printf("no pid file\n");
        else
            printf("STALE - the file names a dead process\n");
    }

    /* vdiscd is per drive - design/09's finding 4, following sysklogd. */
    for (i = 0; i < 4; i++) {
        const char *path = vlhe_status_pidfile_path("vdiscd", i);
        int st = vlhe_status_daemon(path);

        if (st == 0 && i > 0)
            continue;           /* only report drive 0's absence */
        printf("  %-10s %-34s ", "vdiscd", path);
        if (st > 0)
            printf("RUNNING, pid %d\n", st);
        else if (st == 0)
            printf("no pid file\n");
        else
            printf("STALE - the file names a dead process\n");
    }

    printf("\n  A pid file means THE PROCESS EXISTS, never that it is\n");
    printf("  working - vlhe_backend.h is emphatic about that, and the\n");
    printf("  counters that would tell a wedged daemon from a quiet one\n");
    printf("  do not exist (design/09).\n");
    printf("\n  The per-drive files above are design/09's finding 4:\n");
    printf("  vdiscd serves ONE drive, so four discs is four processes.\n");
}

/*
 * THE DISC SIDE - AND THIS SECTION EXISTS BECAUSE THE PROBE COULD NOT
 * ANSWER THE ONE QUESTION IT WAS RUN FOR.
 *
 * 2026-09-21 on 86Box: the module was loaded, vdiscd was running, and
 * the CD page said "no virtual drives are available". The user ran
 * vlhe-probe and it reported MODULES, DAEMONS, MIXER, SOUNDFONTS and
 * CONFIG FILES - every one of them correct, and NONE of them about
 * discs. The cause took four inferences from the code instead.
 *
 * IT WAS ONE NUMBER. /etc/vlhe.conf said `Major = 60', the plan loaded
 * vdisc at 60, and /dev/vdisc0 was still major 63 from an older
 * load.sh run - so the open returned -ENXIO and the drive count was
 * zero. Nothing in the chain was placed to say "the node names a
 * major nobody owns".
 *
 * SO THIS PRINTS THE THREE NUMBERS SIDE BY SIDE - what the config
 * asks for, what the nodes say, and what the module actually
 * registered - because the bug is always a disagreement between them
 * and is invisible in any one of them alone.
 */
static void
probe_drives(void)
{
    struct vlhe_conf cfg;
    int  i, loaded, want_major = -1, want_ndevs = -1;
    int  reg_major = -1;
    FILE *fp;

    rule("VIRTUAL DRIVES");

    /*
     * THE CONFIG FILE DIRECTLY, not through vlhe_backend - which this
     * program deliberately does not link (see the Makefile: it stays
     * small enough to be the only thing staged on a guest, and the
     * backend would drag the daemon control channel in with it).
     *
     * AND READING THE FILE IS THE HONEST THING HERE ANYWAY. The point
     * of this section is to compare what the config SAYS against what
     * the kernel DID; going through a layer that supplies defaults
     * would hide an absent key behind a plausible number, which is
     * the opposite of what a probe is for. The defaults below match
     * vlhe_backend.c:1235 and are printed as defaults when the key is
     * missing.
     */
    cfg.n = 0;
    if (vlhe_conf_read(&cfg, vlhe_conf_system_path()) >= 0) {
        int have_major = vlhe_conf_get(&cfg, "CD Settings",
                                       "Major", NULL) != NULL;
        int have_ndevs = vlhe_conf_get(&cfg, "CD Settings",
                                       "Drives", NULL) != NULL;

        want_major = vlhe_conf_get_int(&cfg, "CD Settings", "Major", 63);
        want_ndevs = vlhe_conf_get_int(&cfg, "CD Settings", "Drives", 1);

        printf("config asks for: major %d%s, %d drive(s)%s\n",
               want_major, have_major ? "" : " (default - key absent)",
               want_ndevs, have_ndevs ? "" : " (default - key absent)");
        printf("                 %s\n", vlhe_conf_system_path());
    } else {
        printf("config: %s could not be read\n",
               vlhe_conf_system_path());
    }

    loaded = vlhe_status_module_loaded("vdisc");
    printf("vdisc module:    %s\n", loaded ? "LOADED" : "not loaded");

    /*
     * WHAT THE KERNEL REGISTERED, from /proc/devices - the only
     * honest answer, since 2.2 has no /sys and a loaded module's
     * parameters cannot be read back.
     */
    fp = fopen("/proc/devices", "r");
    if (fp != NULL) {
        char line[128];
        int  in_block = 0;

        while (fgets(line, sizeof line, fp) != NULL) {
            int  m;
            char name[64];

            if (strncmp(line, "Block devices:", 14) == 0) {
                in_block = 1;
                continue;
            }
            if (!in_block)
                continue;
            if (sscanf(line, "%d %63s", &m, name) == 2
                && strcmp(name, "vdisc") == 0) {
                reg_major = m;
                break;
            }
        }
        fclose(fp);
    }

    if (reg_major >= 0)
        printf("registered at:   major %d  (/proc/devices)\n", reg_major);
    else
        printf("registered at:   NOT IN /proc/devices as a block device\n");

    printf("\n  node             major  minor  opens?\n");

    for (i = 0; i < VLHE_MAX_DRIVE; i++) {
        char path[64];
        struct stat sb;
        int  fd;

        sprintf(path, "/dev/vdisc%d", i);

        if (stat(path, &sb) != 0) {
            if (i < want_ndevs)
                printf("  %-16s ABSENT - the config wants this drive\n",
                       path);
            continue;
        }
        if (!S_ISBLK(sb.st_mode)) {
            printf("  %-16s NOT A BLOCK DEVICE\n", path);
            continue;
        }

        printf("  %-16s %-6d %-6d ", path,
               (int) major(sb.st_rdev), (int) minor(sb.st_rdev));

        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            printf("yes\n");
            close(fd);
        } else if (errno == ENOMEDIUM) {
            /* AN EMPTY DRIVE, NOT AN ABSENT ONE - vdisc_mod.c:722.
             * Conflating the two once made the CD page unusable. */
            printf("yes (no disc)\n");
        } else {
            printf("NO - %s\n", strerror(errno));
        }
    }

    /* The control node, whose minor comes from /proc/misc. */
    {
        struct stat sb;
        int ctl = -1;

        fp = fopen("/proc/misc", "r");
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

        printf("\n  /dev/vdiscctl    ");
        if (stat("/dev/vdiscctl", &sb) == 0 && S_ISCHR(sb.st_mode))
            printf("char %d,%d", (int) major(sb.st_rdev),
                   (int) minor(sb.st_rdev));
        else
            printf("ABSENT");
        if (ctl >= 0)
            printf("   /proc/misc says minor %d\n", ctl);
        else
            printf("   NOT IN /proc/misc\n");
    }

    /*
     * THE VERDICT, because three numbers in a table still need
     * someone to notice they disagree - and on the evening this was
     * written, nobody did for four hours.
     */
    printf("\n");
    if (!loaded) {
        printf("  vdisc is not loaded, so no drive can work.\n");
    } else if (reg_major < 0) {
        printf("  LOADED BUT NOT REGISTERED as a block device - that\n");
        printf("  should not happen; check the load messages.\n");
    } else if (want_major > 0 && reg_major != want_major) {
        printf("  MISMATCH: the config says major %d and the module\n",
               want_major);
        printf("  registered at %d. Whichever the nodes name, one of\n",
               reg_major);
        printf("  the two is wrong - reload, or fix the config.\n");
    } else {
        int bad = 0;

        for (i = 0; i < want_ndevs && i < VLHE_MAX_DRIVE; i++) {
            char path[64];
            struct stat sb;

            sprintf(path, "/dev/vdisc%d", i);
            if (stat(path, &sb) != 0 || !S_ISBLK(sb.st_mode)
                || (int) major(sb.st_rdev) != reg_major)
                bad++;
        }
        if (bad > 0) {
            printf("  %d of the %d configured node(s) do NOT name major\n",
                   bad, want_ndevs);
            printf("  %d. THAT IS THE BUG THIS SECTION WAS ADDED FOR:\n",
                   reg_major);
            printf("  the module is fine and the node points nowhere, so\n");
            printf("  the GUI reports no drives. `vlhe apply' remakes\n");
            printf("  them; load.sh does too.\n");
        } else {
            printf("  Config, module and nodes agree on major %d.\n",
                   reg_major);
        }
    }
}

static void
probe_mixer(void)
{
    struct vlhe_mixer_chan ch[32];
    int n, i;

    rule("MIXER");
    printf("device: %s\n\n", vlhe_mixer_device());

    n = vlhe_mixer_read(ch, 32);
    if (n == 0) {
        printf("  no mixer - no card, or the device is absent.\n");
        printf("  (this is not an error; the GUI still opens)\n");
        return;
    }

    printf("  %-3s %-12s %-6s %-6s %s\n", "id", "name", "left", "right",
           "stereo");
    for (i = 0; i < n; i++) {
        printf("  %-3d %-12s %-6d %-6d %s\n",
               ch[i].id, ch[i].name, ch[i].left, ch[i].right,
               ch[i].stereo ? "yes" : "no");
    }
    printf("\n  %d channels. The card's DEVMASK decides this list;\n", n);
    printf("  the Acer's Solo-1 reports ten, an SB16 fewer.\n");
}

static void
probe_fonts(void)
{
    char dirs[VLHE_MAX_FONTDIRS][VLHE_PATH_MAX];
    char found[VLHE_MAX_AVAIL][VLHE_PATH_MAX];
    int nd = 0, n, i;
    const char *home = getenv("HOME");

    rule("SOUNDFONTS");

    strcpy(dirs[nd++], vlhe_fontscan_defaults[0]);
    strcpy(dirs[nd++], vlhe_fontscan_defaults[1]);
    if (home != NULL && strlen(home) + 12 < VLHE_PATH_MAX) {
        sprintf(dirs[nd], "%s/.vlhe/sf2", home);
        nd++;
    }

    for (i = 0; i < nd; i++)
        printf("  searching: %s\n", dirs[i]);

    /* The cast is C89's doing: it will not implicitly convert
     * char[][N] to const char[][N], where C++ and later C would. */
    n = vlhe_fontscan((const char (*)[VLHE_PATH_MAX])dirs, nd,
                      found, VLHE_MAX_AVAIL);
    printf("\n  %d font(s) found\n", n);
    for (i = 0; i < n; i++)
        printf("    %s\n", found[i]);
    if (n == 0)
        printf("    (expected on a fresh machine - nothing on Corel\n"
               "     installs a SoundFont)\n");
}

static void
probe_config(void)
{
    struct vlhe_conf c;
    const char *sys = vlhe_conf_system_path();
    const char *usr = vlhe_conf_user_path();
    const char *drv = vlhe_conf_drives_path();
    char tmp[256];
    int rc;

    rule("CONFIG FILES");

    printf("  system : %s%s\n", sys,
           vlhe_conf_writable(sys) ? "  (writable)" : "  (NOT writable)");
    printf("  drives : %s%s\n", drv,
           vlhe_conf_writable(drv) ? "  (writable)" : "  (NOT writable)");
    printf("  user   : %s%s\n", usr ? usr : "(no HOME)",
           usr && vlhe_conf_writable(usr) ? "  (writable)" : "");

    /* Read whatever is there - on a fresh machine, nothing. */
    rc = vlhe_conf_read(&c, sys);
    printf("\n  reading %s: %s, %d key(s)\n", sys,
           rc == 0 ? "ok" : "FAILED", rc == 0 ? c.n : 0);

    /* THE WRITE, into /tmp and ONLY there - `-w', which wrote the real
     * system config, is gone (2026-10-02). This exercises the
     * fsync-copy-rename on a real ext2 without changing anything. */
    sprintf(tmp, "/tmp/sysinfo-vlhe-%ld.conf", (long)getpid());
    vlhe_conf_template_defaults(&c, VLHE_TPL_SYSTEM);
    printf("\n  template: %d live key(s) for the system file\n", c.n);

    rc = vlhe_conf_template_write(&c, tmp, VLHE_TPL_SYSTEM, 0);
    printf("  writing %s: %s\n", tmp, rc == 0 ? "ok" : strerror(errno));

    if (rc == 0) {
        struct vlhe_conf back;

        if (vlhe_conf_read(&back, tmp) == 0)
            printf("  reading it back: ok, %d key(s)%s\n", back.n,
                   back.n == c.n ? "" : "  <-- MISMATCH");
        else
            printf("  reading it back: FAILED\n");
        printf("  (left in place so you can look at it)\n");
    }

}

extern char **environ;

/*
 * THE OVERRIDES ROOT WILL IGNORE, named before they are dropped so the
 * report can say they were there. The same names vlhe_self.c's
 * untrusted_name() drops - keep the two in step.
 */
static void
note_ignored(char *out, int max)
{
    static const char *const named[] = {
        "VDISCD_CTL_DIR=", "VMIDID_CTL_DIR=", "MODPATH=", NULL
    };
    char **e;
    int i, hit;

    out[0] = '\0';
#ifndef VLHE_TRUST_ENV_AS_ROOT
    if (getuid() != 0 || geteuid() != 0)
        return;
    for (e = environ; e != NULL && *e != NULL; e++) {
        const char *eq = strchr(*e, '=');
        int n = eq != NULL ? (int) (eq - *e) : (int) strlen(*e);

        hit = strncmp(*e, "VLHE_", 5) == 0;
        for (i = 0; !hit && named[i] != NULL; i++)
            hit = strncmp(*e, named[i], strlen(named[i])) == 0;
        if (hit && (int) strlen(out) + n + 2 < max) {
            strcat(out, " ");
            strncat(out, *e, n);
        }
    }
#endif
}

int
main(int argc, char **argv)
{
    char   report[VLHE_PATH_MAX];
    char   stamp[32];
    char   ignored[512];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int    saved = -1, fd = -1;

    /*
     * ROOT TAKES NO OVERRIDES FROM ITS ENVIRONMENT, as vlhe and
     * vlhe.gtk do not (design/55 recommendation 3, vlhe_self.h) - so
     * the report describes the configuration root's VLHE actually
     * uses. Missing until 2026-10-05 (the G01 documentation review):
     * root with VLHE_CONF set got a report about a file its own
     * `vlhe apply' never reads. First, before anything reads one.
     */
    note_ignored(ignored, sizeof ignored);
    vlhe_self_root_env();

    if (argc > 1) {
        /* NO OPTIONS. `-w' wrote the real /etc/vlhe.conf and is gone. */
        printf("usage: %s\n", argv[0]);
        printf("  writes a report of this machine and VLHE to a file in\n");
        printf("  the current directory (or /tmp), and shows it here.\n");
        return 1;
    }

    /*
     * THE FILE TO SEND. The whole report is written to it, then shown
     * on the screen from it - one copy of the bytes, so what is sent
     * is exactly what was seen. The current directory first, /tmp if
     * that cannot be written.
     */
    if (tm != NULL)
        sprintf(stamp, "%04d%02d%02d-%02d%02d%02d", tm->tm_year + 1900,
                tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
                tm->tm_sec);
    else
        strcpy(stamp, "now");
    sprintf(report, "sysinfo-vlhe-%s.txt", stamp);
    fd = open(report, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        sprintf(report, "/tmp/sysinfo-vlhe-%s.txt", stamp);
        fd = open(report, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd >= 0) {
        fflush(stdout);
        saved = dup(1);
        dup2(fd, 1);
        close(fd);
    }

    printf("sysinfo-vlhe - VLHE SYSTEM REPORT - %s\n", stamp);
    printf("\n");
    printf("WHAT IS IN THIS REPORT, so you know what you are sending:\n");
    printf("  - the kernel version and how it was built, and this copy\n");
    printf("    of VLHE (version, where it runs from)\n");
    printf("  - which sound modules are loaded, and the boot files\n");
    printf("    /etc/modules, /etc/conf.modules and /etc/devices\n");
    printf("  - the sound and CD-ROM device nodes\n");
    printf("  - VLHE's daemons, virtual drives, mixer levels and the\n");
    printf("    SoundFont files it can find\n");
    printf("  - VLHE's configuration file locations, the session file\n");
    printf("    and the end of the daemon log\n");
    printf("It CAN CONTAIN your user name, your home directory and the\n");
    printf("names and paths of your files (disc images, SoundFonts).\n");
    printf("Read it before sending it. Nothing on this machine was\n");
    printf("changed to make it except a scratch file under /tmp.\n");
    if (ignored[0] != '\0') {
        printf("\n");
        printf("Run as root: these were set and IGNORED, as vlhe and\n");
        printf("vlhe.gtk ignore them for root:\n");
        printf(" %s\n", ignored);
    }

    probe_system();
    probe_modules();
    probe_boot_config();
    probe_devices();
    probe_daemons();
    probe_drives();
    probe_mixer();
    probe_fonts();
    probe_config();
    probe_session();

    rule("DONE");
    fflush(stdout);

    /* BACK TO THE SCREEN, and the report shown from the file. */
    if (saved >= 0) {
        FILE *rf;
        char  buf[512];

        dup2(saved, 1);
        close(saved);
        rf = fopen(report, "r");
        if (rf != NULL) {
            while (fgets(buf, sizeof buf, rf) != NULL)
                fputs(buf, stdout);
            fclose(rf);
        }
        printf("\nThe report is in %s - please send that file.\n",
               report);
    } else {
        printf("\n(could not write a report file in the current\n");
        printf(" directory or /tmp - the report above is all there is)\n");
    }
    return 0;
}
