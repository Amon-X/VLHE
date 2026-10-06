/*
 * vlhe_self.c - where is this binary, and is it a trial copy?
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * Read vlhe_self.h first: it has why the working directory cannot be
 * used and why the two questions here are separate.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "vlhe_self.h"

/* RESOLVED ONCE. Nothing about the answer can change while we run -
 * the binary cannot move out from under itself - and three callers
 * ask. */
static char g_dir[VLHE_SELF_MAX];
static int  g_done;

/*
 * STRIP THE FILENAME, leaving a directory with NO trailing slash.
 *
 * No trailing slash because everything downstream appends "/name",
 * and the one that did not was the 2026-09-22 `/mnt/xfer//Roland'
 * bug. One convention, enforced here, rather than a guard at every
 * join.
 */
static void
dirname_of(char *path)
{
    char *slash = strrchr(path, '/');

    if (slash == NULL) {
        strcpy(path, ".");
        return;
    }
    if (slash == path) {
        path[1] = '\0';         /* "/thing" -> "/" */
        return;
    }
    *slash = '\0';
}

/*
 * ARGV[0] AS THE FALLBACK, and only as that.
 *
 * It is what the caller typed, so it can be a bare name found on
 * PATH, a relative path, or a lie - execve lets a caller pass
 * anything. /proc/self/exe is the kernel's own answer and is tried
 * first for that reason.
 *
 * A BARE NAME IS NOT SEARCHED ALONG PATH here. If argv[0] has no
 * slash the program was found on PATH, which means it is INSTALLED,
 * which means the answer to the only question anyone asks of this
 * file is already "not a trial copy". Walking PATH to confirm that
 * would be work to reach a conclusion we have.
 */
static int
from_argv0(const char *argv0, char *out, int max)
{
    char tmp[VLHE_SELF_MAX];

    if (argv0 == NULL || *argv0 == '\0')
        return 0;
    if (strchr(argv0, '/') == NULL)
        return 0;               /* found on PATH - installed */
    if (strlen(argv0) >= sizeof tmp)
        return 0;

    strcpy(tmp, argv0);
    dirname_of(tmp);

    /* RELATIVE argv[0] IS MADE ABSOLUTE AGAINST THE CWD, which is
     * correct: a relative argv[0] was resolved against the cwd by the
     * shell that ran us, and that cwd has not changed yet. This is
     * the ONE place the working directory is legitimately used. */
    if (tmp[0] != '/') {
        char cwd[VLHE_SELF_MAX];

        if (getcwd(cwd, sizeof cwd) == NULL)
            return 0;
        if (strlen(cwd) + 1 + strlen(tmp) >= (size_t) max)
            return 0;
        if (strcmp(tmp, ".") == 0)
            sprintf(out, "%s", cwd);
        else
            sprintf(out, "%s/%s", cwd, tmp);
        return 1;
    }

    if (strlen(tmp) >= (size_t) max)
        return 0;
    strcpy(out, tmp);
    return 1;
}

void
vlhe_self_init(const char *argv0)
{
    char buf[VLHE_SELF_MAX];
    int  n;

    if (g_done)
        return;
    g_done = 1;
    g_dir[0] = '\0';

    /*
     * /proc/self/exe IS THE AUTHORITY - the kernel's own record of
     * what was executed, immune to argv[0] and to the working
     * directory. Present on the target: fs/proc/base.c:118 in the
     * 2.2.16 tree carries PROC_PID_EXE.
     *
     * readlink DOES NOT TERMINATE, which is the classic way to get a
     * path with rubbish on the end.
     */
    n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        dirname_of(buf);
        strncpy(g_dir, buf, sizeof g_dir - 1);
        g_dir[sizeof g_dir - 1] = '\0';
        return;
    }

    /* No /proc, or a kernel without it. */
    if (from_argv0(argv0, buf, sizeof buf)) {
        strncpy(g_dir, buf, sizeof g_dir - 1);
        g_dir[sizeof g_dir - 1] = '\0';
    }

#ifdef VLHE_PORTABLE_BUILD
    /*
     * A PORTABLE BUILD THAT CANNOT FIND ITS FOLDER STOPS HERE. Every path
     * it uses is beside itself; with the folder unknown each one would
     * fall back to the system's - the hybrid this build exists to rule
     * out. Only reached with no /proc AND a bare argv[0]; run it as
     * ./vlhe.gtk or by its full path.
     */
    if (g_dir[0] == '\0') {
        fprintf(stderr, "vlhe: this is a portable copy and it cannot tell"
                        " which folder it is in (no /proc, and it was run"
                        " by a bare name) - run it by its path, e.g."
                        " ./vlhe.gtk\n");
        exit(1);
    }
#endif
}

const char *
vlhe_self_dir(void)
{
    if (!g_done)
        vlhe_self_init(NULL);
    return g_dir[0] != '\0' ? g_dir : NULL;
}

int
vlhe_self_path(const char *name, char *out, int max)
{
    const char *dir = vlhe_self_dir();

    if (dir == NULL || name == NULL || out == NULL || max <= 0)
        return 0;

    /* "/" ALREADY ENDS IN A SEPARATOR - the only directory that
     * does, since dirname_of strips every other one. */
    if (strcmp(dir, "/") == 0) {
        if (1 + strlen(name) >= (size_t) max)
            return 0;
        sprintf(out, "/%s", name);
        return 1;
    }

    if (strlen(dir) + 1 + strlen(name) >= (size_t) max)
        return 0;
    sprintf(out, "%s/%s", dir, name);
    return 1;
}

/*
 * THE MODE IS CHOSEN AT BUILD TIME - design/55 13f, the user's proposal,
 * adopted 2026-10-04. `make portable' compiles with VLHE_PORTABLE_BUILD;
 * `make' (and `make install', the .deb) without it.
 *
 * IT WAS DECIDED BY THE `PORTABLE' FILE, and that made a hybrid possible:
 * delete the file and the SAME folder wrote /etc/vlhe.conf, /var/lib/vlhe
 * and /var/log/vlhe while its programs still came from that folder - an
 * install of files nobody vetted, switched on by one `rm'. The user:
 * "once you delete the PORTABLE stamp it controls the system and writes
 * to the system ... its creating a lot of problems."
 *
 * THE FILE STAYS, AS A LABEL - "It makes it clear what it is for even if
 * it is not used" (the user) - and neither build reads it.
 */

/*
 * THE PORTABLE BUILD'S LAYOUT IS FIXED - "the known subdirectory
 * structure" (the user). It was declared by keys in the marker
 * (Modules=, Daemons=, Tools=); both generators always wrote the same
 * three, so a fixed table loses nothing and removes a file root read
 * from a folder others might write. Any other key - an old SoundFonts=,
 * say - is beside the binary, as an undeclared key always was.
 */
int
vlhe_self_subdir(const char *key, char *out, int max)
{
#ifdef VLHE_PORTABLE_BUILD
    static const char *const map[][2] = {
        { "Modules", "modules" },
        { "Daemons", "daemons" },
        { "Tools",   "tools"   }
    };
    const char *dir, *rel = NULL;
    int i;

    if (key == NULL || out == NULL || max <= 0)
        return 0;
    dir = vlhe_self_dir();
    if (dir == NULL)
        return 0;
    for (i = 0; i < (int) (sizeof map / sizeof map[0]); i++)
        if (strcmp(key, map[i][0]) == 0)
            rel = map[i][1];
    if (rel == NULL) {
        if (strlen(dir) >= (size_t) max)
            return 0;
        strcpy(out, dir);
        return 1;
    }
    return vlhe_self_path(rel, out, max);
#else
    /* THE INSTALLED BUILD HAS NO FOLDER OF ITS OWN to keep things in:
     * its programs, modules and settings are the system's. So nothing
     * is ever looked for beside the binary - not even when a copy of it
     * is run from somewhere else. */
    (void) key;
    (void) out;
    (void) max;
    return 0;
#endif
}

int
vlhe_self_is_trial(void)
{
#ifdef VLHE_PORTABLE_BUILD
    return 1;
#else
    return 0;
#endif
}

/*
 * THE NAMES ROOT DOES NOT TAKE FROM THE ENVIRONMENT - see vlhe_self.h.
 * Every VLHE_* by prefix, so an override added later is covered without
 * anyone remembering this list; the others by name.
 */
static int
untrusted_name(const char *entry)
{
    static const char *const named[] = {
        "VDISCD_CTL_DIR=", "VMIDID_CTL_DIR=", "MODPATH=", NULL
    };
    int i;

    if (strncmp(entry, "VLHE_", 5) == 0)
        return 1;
    for (i = 0; named[i] != NULL; i++)
        if (strncmp(entry, named[i], strlen(named[i])) == 0)
            return 1;
    return 0;
}

extern char **environ;

void
vlhe_self_env_scrub(void)
{
    /* Root's own directories, the same list the setuid build swaps in
     * while raised (vlhe_priv.c, design/48 S5). Static: putenv() keeps
     * the pointer. */
    static char fixed_path[] =
        "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/sbin:/usr/local/bin";
    char name[128];
    int  again = 1;

    /*
     * ONE NAME AT A TIME, RESCANNING AFTER EACH. unsetenv() reshapes
     * environ under the loop, so walking it while removing would skip
     * the entry after each one removed. A name too long for the buffer
     * is left: no variable this program reads is that long, so nothing
     * would ever look it up.
     */
    while (again) {
        char **e;

        again = 0;
        for (e = environ; e != NULL && *e != NULL; e++) {
            const char *eq;
            size_t      n;

            if (!untrusted_name(*e))
                continue;
            eq = strchr(*e, '=');
            n  = eq != NULL ? (size_t) (eq - *e) : strlen(*e);
            if (n == 0 || n >= sizeof name)
                continue;
            memcpy(name, *e, n);
            name[n] = '\0';
            unsetenv(name);
            again = 1;
            break;
        }
    }

    putenv(fixed_path);
}

void
vlhe_self_root_env(void)
{
#ifndef VLHE_TRUST_ENV_AS_ROOT
    if (getuid() == 0 && geteuid() == 0)
        vlhe_self_env_scrub();
#endif
}

/* ------------------------------------------------------------------ */
/* Files in directories others can write - see vlhe_self.h            */
/* ------------------------------------------------------------------ */

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000      /* i386 and x86-64 alike; the target's
                                 * headers give it only under _GNU_SOURCE */
#endif

/* The file we just opened is ours to set: a wide umask must not leave
 * a state file group- or world-writable for the trust check to refuse
 * on the next read. */
static void
tighten(int fd)
{
    struct stat st;

    if (fstat(fd, &st) == 0 && st.st_uid == geteuid()
        && (st.st_mode & 022) != 0)
        (void) fchmod(fd, (st.st_mode & 07777) & ~022);
}

FILE *
vlhe_safe_new(const char *path)
{
    FILE *fp;
    int   fd;

    if (unlink(path) != 0 && errno != ENOENT)
        return NULL;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return NULL;
    tighten(fd);
    fp = fdopen(fd, "w");
    if (fp == NULL)
        close(fd);
    return fp;
}

FILE *
vlhe_safe_append(const char *path)
{
    struct stat st;
    FILE *fp;
    int   fd;

    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0644);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        errno = EPERM;
        return NULL;
    }
    tighten(fd);
    fp = fdopen(fd, "a");
    if (fp == NULL)
        close(fd);
    return fp;
}

FILE *
vlhe_safe_state(const char *path)
{
    struct stat fs, ls;
    FILE *fp;
    int   fd;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &fs) != 0 || lstat(path, &ls) != 0
        || !S_ISREG(fs.st_mode) || S_ISLNK(ls.st_mode)
        || fs.st_dev != ls.st_dev || fs.st_ino != ls.st_ino
        || (fs.st_uid != 0 && fs.st_uid != geteuid())
        || (fs.st_mode & S_IWOTH) != 0) {
        close(fd);
        errno = EPERM;
        return NULL;
    }
    if ((fs.st_mode & S_IWGRP) != 0 && fs.st_gid != 0) {
        /* ONLY ROOT TIGHTENS ROOT'S FILE; anyone else's group-writable
         * file is refused. */
        if (fs.st_uid != geteuid() || fchmod(fd, (fs.st_mode & 07777) & ~022) != 0) {
            close(fd);
            errno = EPERM;
            return NULL;
        }
    }
    fp = fdopen(fd, "r");
    if (fp == NULL)
        close(fd);
    return fp;
}
