/*
 * vlhe_status.c - see vlhe_status.h.
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
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "vlhe_status.h"

const char *
vlhe_status_rundir(void)
{
    const char *v = getenv("VLHE_RUNDIR");

    /*
     * /var/run/vlhe, NOT /var/run - corrected 2026-09-19 after the
     * Status page reported "no pid file" for vdiscd on a machine
     * where vdiscd had just written one. It writes
     * /var/run/vlhe/vdiscd.pid (vdiscd_ctl.c), and this looked in
     * /var/run/vdiscd.pid, so the two never met.
     *
     * A SUBDIRECTORY IS THE RIGHT SHAPE ANYWAY: /var/run is shared
     * with every daemon on the machine, and four files called
     * vsoundd.pid, vmidid.pid, vdiscd.pid and vdiscd.ctl sitting
     * among them is worse than one directory that says whose they
     * are.
     */
    return (v != NULL && *v != '\0') ? v : "/var/run/vlhe";
}

/*
 * WHERE THE DAEMONS' OWN FILES GO - design/55 section 14, design/54 D44,
 * 2026-10-04. The pid files and vsoundd's state are written by the
 * daemons, which run as the `vlhe' account; root's run lock is not.
 * So the account gets a subdirectory, ctl/, and the run directory
 * above it is root's. VLHE_RUNDIR (a test override - root strips every
 * VLHE_*) names ONE directory for both, as it always did.
 */
const char *
vlhe_status_ctldir(void)
{
    const char *v = getenv("VLHE_RUNDIR");

    return (v != NULL && *v != '\0') ? v : "/var/run/vlhe/ctl";
}

/*
 * MAKE A DIRECTORY AND, IF NEED BE, ITS PARENT - design/54 D44. Since
 * 2026-10-04 the control directory is /var/run/vlhe/ctl, a level below
 * the run directory, and on a fresh boot or in a portable copy (no
 * account, so nothing has prepared it) neither exists yet. The parent
 * is made 0755 and left as it is made; an installed machine's daemon
 * start gives it to root (account_dirs()). 0 made or already there.
 */
static int
status_mkdir_up(const char *d, int mode)
{
    char parent[512];
    const char *slash;

    if (mkdir(d, mode) == 0 || errno == EEXIST)
        return 0;
    if (errno != ENOENT)
        return -1;
    slash = strrchr(d, '/');
    if (slash == NULL || slash == d || (size_t) (slash - d) >= sizeof parent)
        return -1;
    memcpy(parent, d, (size_t) (slash - d));
    parent[slash - d] = '\0';
    if (mkdir(parent, 0755) != 0 && errno != EEXIST)
        return -1;
    return (mkdir(d, mode) == 0 || errno == EEXIST) ? 0 : -1;
}

const char *
vlhe_status_modules_path(void)
{
    const char *v = getenv("VLHE_PROC_MODULES");

    return (v != NULL && *v != '\0') ? v : "/proc/modules";
}

const char *
vlhe_status_pidfile_path(const char *name, int drive)
{
    static char buf[256];
    const char *dir = vlhe_status_ctldir();

    if (drive >= 0)
        sprintf(buf, "%.180s/%.40s%d.pid", dir, name, drive);
    else
        sprintf(buf, "%.180s/%.40s.pid", dir, name);
    return buf;
}

/*
 * WRITE THIS PROCESS'S PID FILE. For the daemons, so the Status page
 * can tell "running" from "not running" at all.
 *
 * UNTIL 2026-09-19 NONE OF THEM WROTE ONE, and the Status page said
 * "no pid file" for every daemon on a machine where all three were
 * running - accurate, useless, and the comment in vlhe_backend.c
 * admitted it ("so `not running` here may mean `running and
 * silent`").
 *
 * IT ALSO CLOSES THE GAP vlhe apply's UNLOAD PLAN RECORDS: with no
 * pid file there is nothing to signal, and `killall' is not
 * guaranteed on Corel - psmisc is absent from required_base.
 *
 * Returns 0, or -1 with errno set. A FAILURE IS NOT FATAL to the
 * caller: a daemon that cannot write /var/run still works, it is
 * only harder to see.
 */
int
vlhe_status_write_pidfile(const char *name)
{
    const char *dir = vlhe_status_ctldir();
    struct stat st;
    FILE *fp;

    if (name == NULL)
        return -1;

    /* 0755 - anyone may read a pid; the control channel's own FIFO
     * carries the 0660 that decides who may COMMAND a daemon. */
    if (stat(dir, &st) != 0 && status_mkdir_up(dir, 0755) != 0)
        return -1;

    /*
     * REFUSE IF ONE IS ALREADY RUNNING - and this is the root cause of
     * a real failure, 86Box 2026-09-23, found by the user pressing
     * Load twice.
     *
     * WHAT HAPPENED. A second vdiscd started, could not open
     * /dev/vdiscctl (which admits ONE opener), printed "Device or
     * resource busy" and exited - but only AFTER this function had
     * replaced the first daemon's pid with its own. The pidfile then
     * named a dead process, so the next Unload signalled nothing, the
     * first daemon kept holding the module, `rmmod vdisc' failed, and
     * the journal said "the machine is as it was" over a machine with
     * vdisc still loaded.
     *
     * THE OVERWRITE IS THE MECHANISM AND THE SECOND INSTANCE IS THE
     * CAUSE. Checking here fixes it for every daemon at once rather
     * than in three mains, and it fails in the safe direction: a
     * daemon that will not start twice cannot orphan the one that is
     * already working.
     *
     * A STALE FILE IS NOT A RUNNING DAEMON. vlhe_status_daemon()
     * already distinguishes them - it reads the pid and checks the
     * process is alive - so a crashed daemon's leftover file does not
     * lock the next one out.
     *
     * RETURNS -2, NOT -1, so a caller can tell "one is already
     * running" from "the file could not be written" and say the right
     * thing. Both are failures; only one of them means the machine is
     * fine.
     */
    {
        int live = vlhe_status_daemon(vlhe_status_pidfile_path(name, -1));

        if (live > 0 && live != (int) getpid())
            return -2;
    }

    fp = fopen(vlhe_status_pidfile_path(name, -1), "w");
    if (fp == NULL)
        return -1;

    fprintf(fp, "%lu\n", (unsigned long) getpid());
    return (fclose(fp) == 0) ? 0 : -1;
}

/* Remove it. Called at a clean exit, so a pid file left behind
 * genuinely means the daemon died rather than stopped. */
void
vlhe_status_remove_pidfile(const char *name)
{
    if (name != NULL)
        unlink(vlhe_status_pidfile_path(name, -1));
}

/* Walk /proc/modules, calling back with each line's first field.
 * Returns the use count for `name', or -1 if it is not there. */
static int
module_lookup(const char *name, int *found)
{
    FILE *fp;
    char line[256];
    int users = -1;

    *found = 0;

    fp = fopen(vlhe_status_modules_path(), "r");
    if (fp == NULL)
        return -1;              /* no /proc - nothing is loaded, as far
                                 * as anything here can tell */

    while (fgets(line, sizeof line, fp) != NULL) {
        char mod[64];
        unsigned long size;
        int line_users = -1;
        int n;

        /*
         * "vsound 31184 0" - name, size, use count. Reading the first
         * three fields with a WIDTH on the string, so a malformed or
         * hostile line cannot overflow.
         *
         * INTO A PER-LINE VARIABLE, so the scan cannot write to the
         * result before the name has been checked.
         *
         * THIS IS A TIDY-UP, NOT A BUG FIX, and the distinction is
         * worth keeping because a session got it wrong here. The older
         * version scanned straight into `users' and reset it to -1 on
         * every non-matching line; that reset is correct for every
         * line /proc/modules actually emits, so the two behave
         * identically on this platform. The reset was only skippable
         * via the `n < 1' path, which needs a blank or unparseable
         * line, and 2.2's /proc/modules produces none.
         *
         * It was written believing it fixed a wrong use count seen on
         * 86Box (2026-09-18). It did not - that reading was never
         * diagnosed, and this parser was not the cause. Keeping the
         * shape because a value that cannot be written early is easier
         * to verify than one protected by a reset three lines away.
         */
        n = sscanf(line, "%63s %lu %d", mod, &size, &line_users);
        if (n < 1)
            continue;

        /* EXACT match on the first field. A prefix match would make
         * "vsound" true when only "vsoundfoo" is loaded. */
        if (strcmp(mod, name) != 0)
            continue;

        *found = 1;
        /* present, but the count is unreadable */
        users = (n < 3) ? -1 : line_users;
        break;
    }

    fclose(fp);
    return users;
}

int
vlhe_status_module_loaded(const char *name)
{
    int found = 0;

    module_lookup(name, &found);
    return found;
}

int
vlhe_status_module_users(const char *name)
{
    int found = 0;
    int users = module_lookup(name, &found);

    return found ? users : -1;
}

/*
 * THE BRACKET LIST - who holds this module.
 *
 * vlhe_status.h has why this identifies the user's sound card where
 * nothing else can. The line shape, from 2.2:
 *
 *     soundcore    2788   4 [vsound vmidi es1371]
 *
 * A module with no dependents has no brackets at all, which is not an
 * error - it is the common case for a leaf driver.
 */
int
vlhe_status_module_holders(const char *name, char out[][64], int max)
{
    FILE *fp;
    char  line[512];
    int   n = 0;

    if (name == NULL || out == NULL || max <= 0)
        return 0;

    fp = fopen(vlhe_status_modules_path(), "r");
    if (fp == NULL)
        return 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        char  mod[64];
        char *open_br, *close_br, *p;

        if (sscanf(line, "%63s", mod) != 1)
            continue;
        if (strcmp(mod, name) != 0)
            continue;

        /* MATCHED. The dependents are between the brackets, space
         * separated; no brackets means nothing holds it. */
        open_br = strchr(line, '[');
        if (open_br == NULL)
            break;
        close_br = strchr(open_br, ']');
        if (close_br != NULL)
            *close_br = '\0';

        p = open_br + 1;
        while (n < max) {
            char *end;

            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '\0')
                break;

            end = p;
            while (*end != '\0' && *end != ' ' && *end != '\t')
                end++;
            if (*end != '\0')
                *end++ = '\0';

            strncpy(out[n], p, 63);
            out[n][63] = '\0';
            n++;
            p = end;
        }
        break;                  /* one line per module */
    }

    fclose(fp);
    return n;
}

int
vlhe_status_read_pidfile(const char *path)
{
    FILE *fp;
    char buf[32];
    char *end;
    long v;

    fp = fopen(path, "r");
    if (fp == NULL)
        return 0;

    if (fgets(buf, sizeof buf, fp) == NULL) {
        fclose(fp);
        return 0;               /* empty file - a daemon caught mid-write */
    }
    fclose(fp);

    errno = 0;
    v = strtol(buf, &end, 10);

    /* The platform's own files are "the number and a newline, nothing
     * else", so anything after the digits that is not whitespace means
     * this is not one of ours. */
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != '\0' || errno != 0)
        return 0;

    /* A pid of 0 or below is not a process we can ask about, and
     * passing one to kill() is the accident CLAUDE.md records. */
    if (v <= 0 || v > 0x7fffffffL)
        return 0;

    return (int)v;
}

int
vlhe_status_pid_alive(int pid)
{
    /* THE GUARD, AND IT IS THE POINT OF THIS FUNCTION EXISTING.
     * Signal 0 delivers nothing, but the kernel still reads the pid:
     * 0 means "my whole process group" and -1 means "every process I
     * own". kill(-1, SIGKILL) took the user's X session twice on
     * 2026-09-14 from exactly this shape. */
    if (pid <= 0)
        return 0;

    if (kill(pid, 0) == 0)
        return 1;

    /* EPERM: it exists and belongs to somebody else - which is the
     * normal answer when the GUI runs as a user and the daemon as
     * root, so it must NOT read as dead. */
    return errno == EPERM ? 1 : 0;
}

int
vlhe_status_daemon(const char *pidfile)
{
    int pid = vlhe_status_read_pidfile(pidfile);

    if (pid == 0)
        return 0;               /* no file: never started, or stopped */
    if (vlhe_status_pid_alive(pid))
        return pid;
    return -1;                  /* a file naming a dead process */
}

/*
 * WHICH DSP INDEX VSOUND TOOK - see the header for why this is not
 * an ioctl.
 *
 * The file is `key: value', one per line, written by
 * vsound_proc_get_info(). Only `dsp' is read here; the rest is there
 * for a person and for whatever needs it next.
 */
int
vlhe_vsound_dsp(void)
{
    FILE *fp;
    char  line[128];
    const char *path;
    int   found = -1;

    path = getenv("VLHE_PROC_VSOUND");
    if (path == NULL || *path == '\0')
        path = "/proc/vsound";

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;              /* not loaded, or no proc filesystem */

    while (fgets(line, sizeof line, fp) != NULL) {
        int v;

        if (sscanf(line, "dsp: %d", &v) == 1) {
            found = v;
            break;
        }
    }
    fclose(fp);
    return found;
}

/*
 * RESOLVE `-o' EVERY TIME THE DEVICE IS OPENED - design/43 part A.
 * The header has the contract and why 0 means "wait".
 *
 * WHY THIS EXISTS AT ALL: the daemons re-open their output device
 * repeatedly - vmidid after every release, vdiscd on every play -
 * but until now they re-opened a string resolved ONCE, before exec,
 * by whoever built the plan. So a vsound loaded afterwards was
 * invisible to them, and a vsound that moved was worse.
 *
 * `/dev/dsp' IS DELIBERATELY NOT A FALLBACK HERE. probe_vsound() in
 * vlhe_apply.c ends with one, for a caller that must print
 * SOMETHING; a daemon about to make sound must not, because the
 * answer would be a real card under another name.
 */
int
vlhe_vdisc_position(int index, int *track, int *playing)
{
    FILE *fp;
    char  line[128];
    const char *path;
    int   in = 0, attached = 0, lba = -1, trk = 0, play = 0, v;
    char  st[24];

    path = getenv("VLHE_PROC_VDISC");
    if (path == NULL || *path == '\0')
        path = "/proc/vdisc";

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;              /* no entry: older module, or not loaded */

    /* `drive: N' opens a block; the keys that follow belong to it until
     * the next `drive:'. Only the asked-for block is read. */
    while (fgets(line, sizeof line, fp) != NULL) {
        if (sscanf(line, "drive: %d", &v) == 1) {
            if (in)
                break;          /* past our block */
            in = (v == index);
            continue;
        }
        if (!in)
            continue;
        if (sscanf(line, "attached: %d", &v) == 1)
            attached = v;
        else if (sscanf(line, "lba: %d", &v) == 1)
            lba = v;
        else if (sscanf(line, "track: %d", &v) == 1)
            trk = v;
        else if (sscanf(line, "status: %23s", st) == 1)
            play = (strcmp(st, "play") == 0);
    }
    fclose(fp);

    if (!in || !attached || lba < 0)
        return -1;
    if (track != NULL)
        *track = trk;
    if (playing != NULL)
        *playing = play;
    return lba;
}

int
vlhe_vdisc_proc_int(int index, const char *key)
{
    FILE *fp;
    char  line[128];
    char  want[48];
    const char *path;
    int   in = 0, v, found = -1;

    path = getenv("VLHE_PROC_VDISC");
    if (path == NULL || *path == '\0')
        path = "/proc/vdisc";
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    sprintf(want, "%.40s: %%d", key);
    while (fgets(line, sizeof line, fp) != NULL) {
        if (sscanf(line, "drive: %d", &v) == 1) {
            if (in)
                break;
            in = (v == index);
            continue;
        }
        if (in && sscanf(line, want, &v) == 1) {
            found = v;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* <run dir>/vsoundd.state - design/54 section 6b. */
static const char *
pump_state_path(char *buf, size_t max, int tmp)
{
    const char *dir = vlhe_status_ctldir();   /* the pump writes it */

    if (strlen(dir) + 24 >= max)
        return NULL;
    sprintf(buf, "%s/vsoundd.state%s", dir, tmp ? ".tmp" : "");
    return buf;
}

int
vlhe_pump_state_write(const char *card, int ready)
{
    char  path[256], tmp[256];
    FILE *fp;

    if (card == NULL || pump_state_path(path, sizeof path, 0) == NULL
        || pump_state_path(tmp, sizeof tmp, 1) == NULL)
        return -1;
    /* WRITTEN BESIDE, THEN RENAMED, so a daemon polling it never reads
     * half a file - rename() replaces the old one in one step. */
    fp = fopen(tmp, "w");
    if (fp == NULL)
        return -1;
    fprintf(fp, "pid %lu\ncard %.200s\nstate %s\n",
            (unsigned long) getpid(), card,
            ready ? "ready" : "waiting");
    if (fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void
vlhe_pump_state_remove(void)
{
    char path[256];

    if (pump_state_path(path, sizeof path, 0) != NULL)
        unlink(path);
}

int
vlhe_pump_state(char *card, size_t max)
{
    char  path[256], line[256], cardbuf[220];
    FILE *fp;
    long  pid = 0;
    int   state = -1;

    if (card != NULL && max > 0)
        card[0] = '\0';
    if (pump_state_path(path, sizeof path, 0) == NULL)
        return -1;
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    cardbuf[0] = '\0';
    while (fgets(line, sizeof line, fp) != NULL) {
        char word[16];

        if (sscanf(line, "pid %ld", &pid) == 1)
            continue;
        if (sscanf(line, "card %219s", cardbuf) == 1)
            continue;
        if (sscanf(line, "state %15s", word) == 1)
            state = strcmp(word, "ready") == 0 ? VLHE_PUMP_READY
                  : strcmp(word, "waiting") == 0 ? VLHE_PUMP_WAITING : -1;
    }
    fclose(fp);

    /* A FILE NAMING A DEAD PUMP IS NO PUMP - vsoundd removes it on the
     * way out, but a SIGKILL leaves it. pid > 0 is checked inside. */
    if (pid <= 0 || !vlhe_status_pid_alive((int) pid))
        return -1;
    if (state < 0 || cardbuf[0] == '\0')
        return -1;
    if (card != NULL && max > 0) {
        strncpy(card, cardbuf, max - 1);
        card[max - 1] = '\0';
    }
    return state;
}

int
vlhe_card_open_retryable(int err)
{
    /*
     * ENODEV IS "NOTHING THERE" UNLESS A CARD THAT USES IT FOR "BUSY" IS
     * LOADED - design/55 R11, design/54 D53 (2026-10-04), CLAUDE.md section 5. soundcore answers an
     * empty minor ENODEV (after request_module() for it); only i810_audio
     * and trident return it for every channel taken. Retrying an empty
     * minor meant ~100 opens in the pump's ten-second wait, each one a
     * module request as root.
     */
    if (err == ENOENT || err == ENXIO)
        return 0;
    if (err == ENODEV)
        return vlhe_status_module_loaded("i810_audio")
               || vlhe_status_module_loaded("trident");
    return 1;
}

int
vlhe_vdisc_packet_interface(void)
{
    FILE *fp;
    char  line[128];
    const char *path;
    int   v, found = -1;

    path = getenv("VLHE_PROC_VDISC");
    if (path == NULL || *path == '\0')
        path = "/proc/vdisc";
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    /* The module-wide lines come before the first drive block. */
    while (fgets(line, sizeof line, fp) != NULL) {
        if (sscanf(line, "drive: %d", &v) == 1)
            break;
        if (sscanf(line, "packet_interface: %d", &v) == 1) {
            found = v ? 1 : 0;
            break;
        }
    }
    fclose(fp);
    return found;
}

int
vlhe_vdisc_proc_drives(void)
{
    FILE *fp;
    char  line[128];
    const char *path;
    int   v, n = 0;

    path = getenv("VLHE_PROC_VDISC");
    if (path == NULL || *path == '\0')
        path = "/proc/vdisc";
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    /* One block per advertised drive, attached or not (vdisc_mod.c,
     * vdisc_proc_get_info() walks 0..vdisc_ndevs-1). */
    while (fgets(line, sizeof line, fp) != NULL)
        if (sscanf(line, "drive: %d", &v) == 1)
            n++;
    fclose(fp);
    return n;
}

/*
 * vsound's dsp index, ONLY WHEN ITS PUMP IS READY - design/54 6b step 2,
 * 2026-10-03. Loaded is not enough: vsound refuses a mix-rate change
 * once any channel is open, so a daemon that arrives before vsoundd has
 * the card and has set the rate pins the wrong one - and a vsound with
 * no pump at all discards everything at the clock's rate, silently.
 * -1 when either is missing.
 */
static int
vsound_usable(void)
{
    int idx = vlhe_vsound_dsp();

    if (idx < 0 || vlhe_pump_state(NULL, 0) != VLHE_PUMP_READY)
        return -1;
    return idx;
}

/* Do two paths name the same character device? Followed through any
 * symlink - the load names the stock card /dev/dsp0 and points /dev/dsp
 * elsewhere, so the strings differ when the device does not. */
static int
same_char_device(const char *a, const char *b)
{
    struct stat sa, sb;

    if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
        return 0;
    return S_ISCHR(sa.st_mode) && S_ISCHR(sb.st_mode)
           && sa.st_rdev == sb.st_rdev;
}

/* vsound's node for index `idx' into `out' - 1, or -1 if it does not
 * fit. Index 0 is `/dev/dsp' with no suffix. */
static int
vsound_node(int idx, char *out, size_t max)
{
    if (idx == 0) {
        if (strlen("/dev/dsp") >= max)
            return -1;
        strcpy(out, "/dev/dsp");
        return 1;
    }
    if (idx > 99 || max < sizeof "/dev/dsp99")
        return -1;
    sprintf(out, "/dev/dsp%d", idx);
    return 1;
}

int
vlhe_resolve_out(const char *spec, char *out, size_t max)
{
    int idx;

    if (spec == NULL || out == NULL || max == 0)
        return -1;

    /*
     * `@VSOUND:<path>@' - VSOUND IF LOADED, THAT PATH IF NOT.
     *
     * Checked BEFORE the plain token, because "@VSOUND:" is not a
     * prefix of "@VSOUND@" and a strcmp against the latter would
     * fall through to the passthrough and hand a daemon the literal
     * tag as a filename.
     *
     * NO WAITING IN THIS FORM. The embedded path is the caller's
     * stated second choice, so there is nothing to wait for - which
     * is the whole difference from VLHE_OUT_VSOUND.
     */
    if (strncmp(spec, VLHE_OUT_VSOUND_OR,
                sizeof VLHE_OUT_VSOUND_OR - 1) == 0) {
        const char *fallback = spec + sizeof VLHE_OUT_VSOUND_OR - 1;
        const char *close    = strrchr(fallback, '@');
        size_t      len;

        if (close == NULL || close == fallback)
            return -1;                  /* malformed: no path, or no @ */
        len = (size_t) (close - fallback);

        idx = vsound_usable();
        if (idx >= 0)
            return vsound_node(idx, out, max);
        if (len >= max)
            return -1;
        memcpy(out, fallback, len);
        out[len] = '\0';
        /*
         * THE PUMP IS WAITING FOR THIS VERY CARD - design/54 6b. vsoundd
         * found it busy (the daemon asking is the likely holder) and is
         * retrying; handing it back to the daemon would keep it out for
         * good. So WAIT: the daemon lets go, vsoundd takes the card and
         * says READY, and the next ask moves the daemon to vsound. If
         * vsoundd gives up it removes its state file and the card comes
         * back here. Compared by device, not by name.
         */
        {
            char pcard[220];

            if (vlhe_pump_state(pcard, sizeof pcard) == VLHE_PUMP_WAITING
                && same_char_device(pcard, out))
                return 0;
        }
        return 1;
    }

    if (strcmp(spec, VLHE_OUT_VSOUND) != 0) {
        /* An ordinary path. Copied rather than returned by pointer so
         * every caller holds its own buffer and none of them has to
         * think about the argv string's lifetime. */
        if (strlen(spec) >= max)
            return -1;
        strcpy(out, spec);
        return 1;
    }

    /* NOT LOADED, OR ITS PUMP IS NOT READY - the caller waits
     * (design/54 6b: a daemon that arrived between insmod and vsoundd
     * setting the rate would pin the wrong one). */
    idx = vsound_usable();
    if (idx < 0)
        return 0;

    /* INDEX 0 IS `/dev/dsp' WITH NO SUFFIX, which is the one case
     * sprintf("%d") gets wrong - vsound_node(). Bounded by the chain:
     * register_sound_dsp() allocates minors 3..115 in steps of 16, so
     * the index is 0..7; the test is against `max' anyway. */
    return vsound_node(idx, out, max);
}

int
vlhe_midi_seqdev(void)
{
    FILE *fp;
    char  line[128];
    const char *path;
    int   found = -1;

    /*
     * ASK vmidi, NOT `/proc/sound' - CHANGED 2026-09-26.
     *
     * This parsed `/proc/sound''s `Midi devices:' section for the
     * line named `vmidi' and returned its index. One number, out of
     * a file whose other sections we never read.
     *
     * AND READING THAT FILE CRASHES SOME MACHINES. The kernel builds
     * ALL of it on the open, so the audio section runs whatever we
     * came for: `sound_proc_get_info()' walks `audio_devs[]' to
     * `num_audiodevs', and `sound_alloc_audiodev()' can set that
     * count past the array's five entries because it derives its
     * index from the shared dsp minor chain with no bound test.
     * Three es1371 plus `sb' was enough on 86Box - the oops is at
     * `audio_devs[5]', which IS `num_audiodevs'. design/36 row 84
     * and tests/logs/2026-09-26-oops-ksyms-preload/ have it.
     *
     * A 1999 KERNEL BUG, NOT OURS - and we were the only program on
     * that machine opening the file, so this removes us from it
     * rather than fixing it. Anything else that reads `/proc/sound'
     * on such a machine still crashes.
     *
     * NO FALLBACK TO THE OLD PATH, the user's call: the modules and
     * the GUI are rebuilt together every staging, so a GUI cannot
     * meet an older `vmidi.o'. A fallback would also reinstate the
     * exact read this exists to avoid, on the machines where it is
     * dangerous.
     *
     * SAME SHAPE AS `vlhe_vsound_dsp()' ABOVE, which asks
     * `/proc/vsound' for the same reason (row 78): the module knows
     * the number, so parsing a listing to re-derive it is inference
     * where an answer is available.
     */
    path = getenv("VLHE_PROC_VMIDI");
    if (path == NULL || *path == '\0')
        path = "/proc/vmidi";

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;              /* not loaded, or no proc filesystem */

    while (fgets(line, sizeof line, fp) != NULL) {
        int v;

        if (sscanf(line, "midi: %d", &v) == 1) {
            found = v;
            break;
        }
    }
    fclose(fp);
    return found;
}
