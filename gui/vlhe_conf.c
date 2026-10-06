/*
 * vlhe_conf.c - reading and writing the config files.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * See vlhe_conf.h for the format, the three files and why the write
 * sequence is shaped the way it is.
 *
 * C89, GCC 2.95.2. No GTK, no driver headers - this is file I/O and
 * nothing else, so it builds and runs on the workstation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include "vlhe_conf.h"
#include "vlhe_self.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000      /* i386 and x86-64 alike; the target's
                                 * headers give it only under _GNU_SOURCE */
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Copy with a guaranteed terminator. strncpy does not give one when
 * the source is longer, and every buffer here is fixed. */
static void
copy_bounded(char *dst, const char *src, size_t size)
{
    size_t n;

    if (size == 0)
        return;
    n = strlen(src);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Trim ASCII blanks from both ends, in place. */
static char *
trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '\0')
        return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' ||
                       *end == '\r' || *end == '\n'))
        end--;
    end[1] = '\0';
    return s;
}

/* Case-insensitive compare. Section and key names are matched this
 * way because a user typing `midichannel' means the same thing, and
 * this file is meant to be hand-editable. Written out rather than
 * using strcasecmp, which is not C89. */
static int
same_name(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (unsigned char)*a;
        int cb = (unsigned char)*b;

        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

/* The parse, from an open stream. Both readers funnel through here. */
static void
read_fp(struct vlhe_conf *c, FILE *fp)
{
    char line[VLHE_CONF_VAL_MAX + VLHE_CONF_KEY_MAX + 64];
    char section[VLHE_CONF_SEC_MAX];

    section[0] = '\0';

    while (fgets(line, sizeof line, fp) != NULL) {
        char *s = trim(line);
        char *eq;

        if (*s == '\0' || *s == ';' || *s == '#')
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');

            /* A section line with no ] is malformed; skip it and keep
             * the section we were in, rather than losing the keys that
             * follow to a half-parsed name. */
            if (close == NULL)
                continue;
            *close = '\0';
            copy_bounded(section, trim(s + 1), sizeof section);
            continue;
        }

        eq = strchr(s, '=');
        if (eq == NULL)
            continue;           /* not a key line - skip, do not fail */
        *eq = '\0';

        if (c->n >= VLHE_CONF_MAX_ENTRIES)
            break;              /* full: keep what we have */

        copy_bounded(c->entry[c->n].section, section,
                     sizeof c->entry[c->n].section);
        copy_bounded(c->entry[c->n].key, trim(s),
                     sizeof c->entry[c->n].key);
        copy_bounded(c->entry[c->n].value, trim(eq + 1),
                     sizeof c->entry[c->n].value);

        /* A key with no name is not a key. */
        if (c->entry[c->n].key[0] != '\0')
            c->n++;
    }
}

int
vlhe_conf_read(struct vlhe_conf *c, const char *path)
{
    FILE *fp;

    c->n = 0;
    c->dirty = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        /* MISSING IS NOT AN ERROR - every key has a default, and a
         * machine with no config file has to work. Only a file that
         * exists and cannot be opened is a failure. */
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    read_fp(c, fp);
    fclose(fp);
    return 0;
}

/*
 * READ A FILE SOMEONE ELSE HANDED US - design/49 T0, Load config. A
 * regular file only (a FIFO would block, a device would read forever),
 * opened non-blocking so even the open cannot hang, and parsed by the
 * same bounded reader as everything else. Nothing here is trusted;
 * the caller checks every value it takes.
 */
int
vlhe_conf_read_regular(struct vlhe_conf *c, const char *path)
{
    struct stat st;
    FILE *fp;
    int fd;

    c->n = 0;
    c->dirty = 0;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    fp = fdopen(fd, "r");
    if (fp == NULL) {
        close(fd);
        return -1;
    }
    read_fp(c, fp);
    fclose(fp);
    return 0;
}

static int uid_name(long want, char *out, int max);

/* Could anyone but root have written this? 0 if not, else 1 with the
 * reason. `what' names it in the message: "file" or "directory". */
static int
untrusted_stat(const struct stat *st, const char *what, const char *path,
               char *why, int max)
{
    char msg[80];

    msg[0] = '\0';
    if (st->st_uid != 0) {
        /* NAMED, so the refusal says whose file it is. */
        char who[40];

        if (uid_name((long) st->st_uid, who, (int) sizeof who) != 0)
            strcpy(who, "another user");
        sprintf(msg, "is owned by %s, not root", who);
    }
    else if ((st->st_mode & S_IWOTH) && !(st->st_mode & S_ISVTX))
        strcpy(msg, "is writable by any user");
    else if ((st->st_mode & S_IWGRP) && st->st_gid != 0
             && !(st->st_mode & S_ISVTX))
        strcpy(msg, "is writable by its group");
    if (msg[0] == '\0')
        return 0;

    /* The sticky bit excuses a DIRECTORY only - it stops others
     * replacing a root-owned file in it. On a file it means nothing,
     * so a file is judged without it. */
    if (why != NULL && max > 0) {
        if ((int) (strlen(what) + strlen(path) + strlen(msg) + 4) < max)
            sprintf(why, "%s %s %s", what, path, msg);
        else
            copy_bounded(why, msg, (size_t) max);
    }
    return 1;
}

int
vlhe_conf_read_trusted(struct vlhe_conf *c, const char *path,
                       char *why, int max)
{
    struct stat fst, lst, dst;
    char dir[PATH_MAX];
    FILE *fp;
    int fd;
    int bad = 0;

    c->n = 0;
    c->dirty = 0;
    if (why != NULL && max > 0)
        why[0] = '\0';

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;     /* missing: the defaults */

    /* THE FILE, BY THE DESCRIPTOR THAT IS PARSED BELOW - and the path
     * must name that same inode and not be a link, so what was judged
     * and what is read cannot differ. */
    if (fstat(fd, &fst) != 0 || lstat(path, &lst) != 0) {
        close(fd);
        return -1;
    }
    if (S_ISLNK(lst.st_mode) || !S_ISREG(fst.st_mode)
        || lst.st_dev != fst.st_dev || lst.st_ino != fst.st_ino) {
        if (why != NULL && max > 0)
            copy_bounded(why, "the configuration is not a plain file",
                         (size_t) max);
        bad = 1;
    } else {
        struct stat f = fst;

        f.st_mode &= ~S_ISVTX;
        bad = untrusted_stat(&f, "file", path, why, max);
    }

    /*
     * EVERY DIRECTORY UP TO /, because whoever can write one can
     * rename what is below it - the file, or a whole directory
     * holding it - and put another in its place. Sticky and root's is
     * fine: /mnt/xfer (1777) cannot have a root-owned file taken from
     * it. THE REAL PATH, so a symlinked directory is judged where it
     * is rather than where the link sits.
     */
    if (!bad) {
        char *slash;

        if (realpath(path, dir) == NULL) {
            if (why != NULL && max > 0)
                copy_bounded(why, "its directory cannot be resolved",
                             (size_t) max);
            bad = 1;
        }
        while (!bad) {
            slash = strrchr(dir, '/');
            if (slash == NULL)
                break;                  /* realpath is absolute */
            if (slash == dir)
                dir[1] = '\0';         /* the root itself */
            else
                *slash = '\0';
            if (stat(dir, &dst) != 0) {
                if (why != NULL && max > 0)
                    copy_bounded(why, "a directory above it cannot be"
                                 " examined", (size_t) max);
                bad = 1;
            } else {
                bad = untrusted_stat(&dst, "directory", dir, why, max);
            }
            if (strcmp(dir, "/") == 0)
                break;
        }
    }

    fp = fdopen(fd, "r");
    if (fp == NULL) {
        close(fd);
        return -1;
    }
    read_fp(c, fp);
    fclose(fp);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                             */
/* ------------------------------------------------------------------ */

static int
find_entry(const struct vlhe_conf *c, const char *section, const char *key)
{
    int i;

    for (i = 0; i < c->n; i++) {
        if (same_name(c->entry[i].section, section) &&
            same_name(c->entry[i].key, key))
            return i;
    }
    return -1;
}

const char *
vlhe_conf_get(const struct vlhe_conf *c, const char *section,
              const char *key, const char *dflt)
{
    int i = find_entry(c, section, key);

    return i < 0 ? dflt : c->entry[i].value;
}

int
vlhe_conf_get_int(const struct vlhe_conf *c, const char *section,
                  const char *key, int dflt)
{
    const char *v = vlhe_conf_get(c, section, key, NULL);
    char *end;
    long n;

    if (v == NULL || *v == '\0')
        return dflt;

    errno = 0;
    n = strtol(v, &end, 10);

    /* Trailing text means this is not a number - `1 (the default)'
     * parses as 1 under a lenient reading, and that is exactly the
     * kind of half-answer that hides a typo. Take the default. */
    end = trim(end);
    if (*end != '\0' || errno != 0)
        return dflt;
    if (n > 0x7fffffffL || n < (-0x7fffffffL - 1))
        return dflt;

    return (int)n;
}

/* ------------------------------------------------------------------ */
/* Setting                                                            */
/* ------------------------------------------------------------------ */

int
vlhe_conf_set(struct vlhe_conf *c, const char *section,
              const char *key, const char *value)
{
    int i = find_entry(c, section, key);

    if (i < 0) {
        if (c->n >= VLHE_CONF_MAX_ENTRIES)
            return -1;
        i = c->n++;
        copy_bounded(c->entry[i].section, section,
                     sizeof c->entry[i].section);
        copy_bounded(c->entry[i].key, key, sizeof c->entry[i].key);
        c->dirty = 1;                   /* a key that was not there */
    } else if (strcmp(c->entry[i].value, value) != 0) {
        c->dirty = 1;                   /* a value that MOVED       */
    }

    /*
     * THE COMPARISON ABOVE IS THE WHOLE POINT. Storing the same value
     * again must NOT mark the file dirty: a GUI that reads its
     * widgets back into the conf on every Apply would otherwise
     * always look changed, and the `.old' backup would be destroyed
     * by a user who changed nothing. See struct vlhe_conf's `dirty'.
     *
     * Note the truncation case is handled by comparing BEFORE the
     * copy: if `value' is longer than the field, the stored result
     * differs from what was asked for, the strcmp above already saw a
     * difference, and dirty is set. A later set of the same long
     * value compares equal against the TRUNCATED text and correctly
     * does nothing.
     */
    copy_bounded(c->entry[i].value, value, sizeof c->entry[i].value);
    return 0;
}

int
vlhe_conf_set_int(struct vlhe_conf *c, const char *section,
                  const char *key, int value)
{
    char buf[32];

    sprintf(buf, "%d", value);
    return vlhe_conf_set(c, section, key, buf);
}

int
vlhe_conf_unset(struct vlhe_conf *c, const char *section, const char *key)
{
    int i = find_entry(c, section, key);

    if (i < 0)
        return -1;

    /* Shift down. The tables are tens of entries, so this is cheaper
     * than a tombstone the writer would have to know about. */
    while (i + 1 < c->n) {
        c->entry[i] = c->entry[i + 1];
        i++;
    }
    c->n--;
    c->dirty = 1;                       /* removing one IS a change */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Writing - the sequence design/33 section 3 settled                 */
/* ------------------------------------------------------------------ */

/* Copy a file, for the .old backup. Returns 0, or -1 with errno set.
 * A source that does not exist is success with nothing done - there is
 * no previous version to keep.
 *
 * NOT STATIC SINCE 2026-09-19: the RESTORE path needs the same copy,
 * and duplicating a read/write loop so one caller could stay private
 * would be the worse trade. Named vlhe_conf_copy in the header. */
int
vlhe_conf_copy(const char *from, const char *to)
{
    int in, out, rc = 0;
    char buf[4096];
    int n;

    in = open(from, O_RDONLY);
    if (in < 0)
        return errno == ENOENT ? 0 : -1;

    /* CREATED FRESH, NEVER THROUGH A LINK - design/55 recommendation 2
     * (R3). `to' is a .old beside a file in a folder a user may own, or
     * the live file Restore Backup copies back over; O_TRUNC followed a
     * symlink planted at the name. Unlink (which never follows) and
     * create exclusively instead. */
    if (unlink(to) != 0 && errno != ENOENT) {
        close(in);
        return -1;
    }
    out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            rc = -1;
            break;
        }
    }
    if (n < 0)
        rc = -1;

    close(in);
    if (close(out) < 0)
        rc = -1;
    return rc;
}

/* Write one section's keys. Returns -1 on any write failure. */
static int
write_section(FILE *fp, const struct vlhe_conf *c, const char *section,
              int *written)
{
    int i;
    int header_done = 0;

    for (i = 0; i < c->n; i++) {
        if (!same_name(c->entry[i].section, section))
            continue;
        if (written[i])
            continue;

        if (!header_done) {
            if (section[0] != '\0') {
                if (fprintf(fp, "\n[%s]\n", section) < 0)
                    return -1;
            }
            header_done = 1;
        }
        if (fprintf(fp, "%s = %s\n",
                    c->entry[i].key, c->entry[i].value) < 0)
            return -1;
        written[i] = 1;
    }
    return 0;
}

/* The write sequence, once, with the caller supplying the body. Both
 * public writers funnel through here so there is ONE crash-safe path
 * rather than two that can drift. */
static int
write_sequence(const char *path, const char *header,
               const struct vlhe_conf *c, vlhe_conf_body_fn fn, int arg,
               int backup)
{
    char tmp[VLHE_CONF_VAL_MAX];
    char old[VLHE_CONF_VAL_MAX];
    FILE *fp;
    int fd;
    int i;
    int written[VLHE_CONF_MAX_ENTRIES];
    int saved_errno;

    if (strlen(path) + 5 >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    sprintf(tmp, "%s.tmp", path);
    sprintf(old, "%s.old", path);

    if (vlhe_conf_mkdir_for(path) < 0)
        return -1;

    /* 1. Write the temporary file - created fresh, never through a link
     *    someone left at the name (design/55 recommendation 2, R3). */
    fp = vlhe_safe_new(tmp);
    if (fp == NULL)
        return -1;

    if (header != NULL && header[0] != '\0') {
        if (fputs(header, fp) < 0)
            goto fail;
    }

    if (fn != NULL) {
        if (fn(fp, c, arg) < 0)
            goto fail;
    } else if (c != NULL) {
        for (i = 0; i < c->n; i++)
            written[i] = 0;

        /* Keys with no section first, so they are not swallowed by
         * whichever section header happens to precede them. */
        if (write_section(fp, c, "", written) < 0)
            goto fail;

        for (i = 0; i < c->n; i++) {
            if (written[i])
                continue;
            if (write_section(fp, c, c->entry[i].section, written) < 0)
                goto fail;
        }
    }

    /* 2. THE fsync, BEFORE ANYTHING POINTS AT THIS FILE. See the
     *    header: without it a power loss after the rename can leave a
     *    valid inode whose contents are nulls. */
    if (fflush(fp) != 0)
        goto fail;
    fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        /* EINVAL means the filesystem does not support it, which is
         * not a reason to lose the write - only a real I/O error is. */
        if (errno != EINVAL)
            goto fail;
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        goto fail;
    }
    fp = NULL;

    /* 3. Copy the current file aside. A COPY, so `path' stays present
     *    and complete until the rename below replaces it. */
    if (backup && vlhe_conf_copy(path, old) < 0)
        goto fail;

    /* 4. The atomic switch. */
    if (rename(tmp, path) != 0)
        goto fail;

    return 0;

fail:
    saved_errno = errno;
    if (fp != NULL)
        fclose(fp);
    unlink(tmp);
    errno = saved_errno;
    return -1;
}

int
vlhe_conf_write_fn(const char *path, const char *header,
                   vlhe_conf_body_fn fn, const struct vlhe_conf *c,
                   int arg, int backup)
{
    return write_sequence(path, header, c, fn, arg, backup);
}

int
vlhe_conf_write(const struct vlhe_conf *c, const char *path,
                const char *header, int backup)
{
    int rc = write_sequence(path, header, c, NULL, 0, backup);

    /*
     * CLEARED ONLY ON SUCCESS, AND `c' IS CAST TO DO IT.
     *
     * The const is right for callers - writing must not alter the
     * settings - but `dirty' is bookkeeping ABOUT the struct rather
     * than content of it, and a failed write must leave it set or the
     * next Apply would skip the retry and silently lose the edit.
     */
    if (rc == 0)
        ((struct vlhe_conf *) c)->dirty = 0;
    return rc;
}


/* ------------------------------------------------------------------ */
/* Paths                                                              */
/* ------------------------------------------------------------------ */

/* THE PATHS ARE OVERRIDABLE BY ENVIRONMENT, and that is for US rather
 * than for users: it is what lets this be exercised on the workstation
 * without writing to the host's /etc. A shipped machine sets none of
 * them and gets the compiled-in paths. */
static const char *
path_or_env(const char *env, const char *dflt)
{
    const char *v = getenv(env);

    return (v != NULL && *v != '\0') ? v : dflt;
}

/*
 * IS THIS MACHINE INSTALLED? The installer writes `[Paths] ModuleDir'
 * into /etc/vlhe.conf, and its presence is the answer - see that
 * key's comment in vlhe_conf_template.c for why a probe was the
 * wrong question.
 *
 * READ BY HAND, NOT THROUGH vlhe_conf_read(), because the config
 * path resolver below needs this and reading the config through the
 * resolver would recurse. A dozen lines of fgets against one file we
 * know the format of, versus an ordering problem - the lines win.
 *
 * NO LONGER USED BY THE PATH RESOLVERS, 2026-09-22, and PUBLIC now
 * rather than deleted. The portable question stopped needing it: a
 * PORTABLE marker beside the binary decides that on its own, and
 * whether the machine is ALSO installed is a separate fact.
 *
 * IT IS EXACTLY THAT SEPARATE FACT THAT IS WANTED NEXT - the first
 * run of a portable copy on an installed machine should offer to copy
 * the existing settings in, and this is how it knows to ask. Deleting
 * a correct twelve-line reader to re-add it in the following commit
 * would be churn.
 */
int
vlhe_conf_machine_installed(void)
{
    FILE *fp;
    char  line[512];
    int   in_paths = 0;
    int   found = 0;

    fp = fopen("/etc/vlhe.conf", "r");
    if (fp == NULL)
        return 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '[') {
            in_paths = strncmp(p, "[Paths]", 7) == 0;
            continue;
        }
        if (!in_paths)
            continue;
        if (strncmp(p, "ModuleDir", 9) != 0)
            continue;

        /* PRESENT AND NON-EMPTY. `ModuleDir =' with nothing after it
         * is the template's own default and means not installed. */
        p = strchr(p, '=');
        if (p == NULL)
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '\0' && *p != '\n' && *p != '\r')
            found = 1;
        break;
    }

    fclose(fp);
    return found;
}

const char *
vlhe_conf_system_path(void)
{
    /*
     * TRIAL MODE PUTS THE CONFIG IN THE FOLDER, and this is the one
     * place that decides it for every program at once.
     *
     * THE USER'S REQUIREMENT, 2026-09-21: someone unzips a prebuilt
     * archive and runs the GUI without installing. Writing /etc then
     * would be exactly what they did not ask for - and deleting the
     * folder must remove every trace, which it cannot if settings are
     * elsewhere.
     *
     * THE TEST IS `vsound.o BESIDE US AND NOTHING INSTALLED', the
     * same one vlhe_where_are_modules() makes. IT IS DUPLICATED HERE
     * ON PURPOSE: this file is a leaf that vlhe-probe links WITHOUT
     * vlhe_apply.c, so it cannot call that function, and a config
     * path that depended on the plan builder would invert the
     * dependency between the two.
     *
     * INSTALLED WINS - the user's call, because installed modules
     * "are most likely rebuilt for that machine". So a machine with
     * /lib/modules/<uname -r>/misc/vsound.o uses /etc even when run
     * from an unpacked folder, which is the upgrade case.
     *
     * VLHE_CONF STILL OVERRIDES BOTH - the host tests depend on it.
     */
    const char *v = getenv("VLHE_CONF");

    if (v != NULL && *v != '\0')
        return v;

    /*
     * THE PORTABLE MARKER WINS, AND THAT REVERSES A RECORDED
     * DECISION - 2026-09-22, and the reversal is the user's.
     *
     * THIS FUNCTION USED TO SAY "INSTALLED WINS", on the user's
     * reasoning at the time that installed modules "are most likely
     * rebuilt for that machine", with the upgrade case cited as the
     * reason. They named the same case again and decided it the other
     * way: someone who has VLHE installed and unpacks a newer build
     * "to try it out to see if it addresses any issues they have"
     * wants THAT tree's config, not the installed one - otherwise the
     * new binary reads the old settings while its own sit unread
     * beside it, and whatever they were testing is confounded.
     *
     * WHAT CHANGED IS THE EVIDENCE, WHICH IS WHY BOTH CALLS WERE
     * REASONABLE. The old test was `./vsound.o' - a GUESS about
     * layout, made against the WORKING directory, so honouring it
     * over an installed machine would have been honouring an
     * accident. The marker is a DECLARATION, found beside the binary
     * rather than beside the cwd: the tarball says what it is, and
     * deferring to that is deferring to the person who ran it.
     *
     * A PORTABLE TREE IS SELF-CONTAINED. Config, user settings,
     * journal and backups all in the folder; nothing installed is
     * read or written, and deleting the folder ends it.
     *
     * VLHE_CONF STILL OVERRIDES EVERYTHING - the host tests depend
     * on it.
     */
    if (vlhe_self_is_trial()) {
        static char buf[VLHE_CONF_VAL_MAX];

        if (vlhe_self_path("vlhe.conf", buf, sizeof buf))
            return buf;
        /* The directory is unknown, which means no /proc and a bare
         * argv[0] - i.e. found on PATH, i.e. installed. Fall through
         * rather than return a relative path, which is the bug this
         * whole change exists to remove. */
    }

    return "/etc/vlhe.conf";
}

/*
 * THE DRIVE STATE - what is in each drive and whether it comes back at
 * the next load. vdiscd writes it (design/33 section 3i); the plan
 * hands it this path. A PORTABLE FOLDER KEEPS ITS OWN, beside itself
 * like `mixers' and `volumes' - the user, 2026-10-03: "do it on load
 * similar to the mixer" - because a copy that leaves the system as it
 * found it cannot write /var/lib.
 */
const char *
vlhe_conf_drives_path(void)
{
    const char *env = getenv("VLHE_DRIVES");

    if (env == NULL || *env == '\0') {
        static char buf[VLHE_CONF_VAL_MAX];

        if (vlhe_self_is_trial()
            && vlhe_self_path("drives", buf, sizeof buf))
            return buf;
    }
    /* IN state/, THE ACCOUNT'S, SINCE 2026-10-04 - design/55 section
     * 14: vdiscd rewrites it by rename, so the account needs its
     * DIRECTORY, and /var/lib/vlhe above it holds root's session and
     * baseline. An older install's /var/lib/vlhe/drives is moved here
     * by the first daemon start (account_dirs()). */
    return path_or_env("VLHE_DRIVES", "/var/lib/vlhe/state/drives");
}

/* --- the daemons' account, without NSS ---------------------------- */

/* Field `n' (0-based) of a colon-separated line, terminated in place;
 * NULL when the line has fewer. The line is the caller's copy. */
static char *
colon_field(char *line, int n)
{
    char *p = line;

    while (n-- > 0) {
        p = strchr(p, ':');
        if (p == NULL)
            return NULL;
        p++;
    }
    {
        char *e = strchr(p, ':');

        if (e != NULL)
            *e = '\0';
        else {
            e = p + strlen(p);
            while (e > p && (e[-1] == '\n' || e[-1] == '\r'))
                *--e = '\0';
        }
    }
    return p;
}

int
vlhe_conf_account(const char *name, struct vlhe_account *a)
{
    FILE  *fp;
    char   line[1024], copy[1024];
    size_t nl;
    int    found = 0;

    if (name == NULL || *name == '\0' || a == NULL)
        return -1;
    memset(a, 0, sizeof *a);
    nl = strlen(name);

    fp = fopen(path_or_env("VLHE_PASSWD", "/etc/passwd"), "r");
    if (fp == NULL)
        return -1;
    while (fgets(line, sizeof line, fp) != NULL) {
        char *u, *g;

        if (strncmp(line, name, nl) != 0 || line[nl] != ':')
            continue;
        strcpy(copy, line);
        u = colon_field(copy, 2);
        strcpy(copy, line);
        g = colon_field(copy, 3);
        if (u == NULL || g == NULL || *u == '\0' || *g == '\0')
            break;
        a->uid = strtol(u, NULL, 10);
        a->gid = strtol(g, NULL, 10);
        a->groups[a->ngroups++] = a->gid;
        found = 1;
        break;
    }
    fclose(fp);
    if (!found)
        return -1;

    /* THE SUPPLEMENTARY GROUPS - every group whose fourth field names
     * the account. A missing /etc/group leaves the primary alone. */
    fp = fopen(path_or_env("VLHE_GROUP", "/etc/group"), "r");
    if (fp == NULL)
        return 0;
    while (fgets(line, sizeof line, fp) != NULL
           && a->ngroups < VLHE_ACCOUNT_GROUPS) {
        char *gid_s, *mem, *tok;
        long  gid;
        int   k, dup = 0;

        strcpy(copy, line);
        gid_s = colon_field(copy, 2);
        if (gid_s == NULL || *gid_s == '\0')
            continue;
        gid = strtol(gid_s, NULL, 10);
        strcpy(copy, line);
        mem = colon_field(copy, 3);
        if (mem == NULL)
            continue;
        for (k = 0; k < a->ngroups; k++)
            if (a->groups[k] == gid)
                dup = 1;
        if (dup)
            continue;
        for (tok = strtok(mem, ","); tok != NULL; tok = strtok(NULL, ","))
            if (strcmp(tok, name) == 0) {
                a->groups[a->ngroups++] = gid;
                break;
            }
    }
    fclose(fp);
    return 0;
}


/* --- who the user is, without NSS --------------------------------- */

/*
 * THE LOGIN NAME OF THE REAL UID, READ STRAIGHT FROM /etc/passwd.
 *
 * NOT getpwuid(): vlhe and vlhe-probe are STATIC binaries, and a
 * static glibc 2.1 getpwuid() dlopen()s libnss_files.so at runtime -
 * it works on the target by accident of that library being present
 * and fails in every environment where it is not. Six fields, one
 * strcmp on the third, is the whole of what we need.
 *
 * NOT getlogin(): it consults utmp and fails under X and under sudo.
 * NOT geteuid(): vlhe.gtk.su is euid 0 after Modify and would name the
 * file "root" for every user.
 *
 * The name is SANITISED to [A-Za-z0-9._-] because it becomes part of
 * a filename in a directory other users can write to. A uid with no
 * passwd entry - a stripped container, a broken NIS - falls back to
 * "uid<N>", which is unambiguous and never collides with a real name.
 */
int
vlhe_conf_user_name(char *out, int max)
{
    return uid_name((long) getuid(), out, max);
}

/* The same lookup for any uid - also used to say who owns a file root
 * refuses to act on (design/49 T0). */
static int
uid_name(long want, char *out, int max)
{
    FILE *fp;
    char  line[512];
    int   got = 0;

    if (out == NULL || max <= 0)
        return -1;
    out[0] = '\0';

    fp = fopen("/etc/passwd", "r");
    if (fp != NULL) {
        while (fgets(line, sizeof line, fp) != NULL) {
            char *f1 = line;
            char *c  = strchr(line, ':');
            char *f3;
            long  uid;

            if (c == NULL) continue;
            *c = '\0';
            f3 = strchr(c + 1, ':');          /* skip the password field */
            if (f3 == NULL) continue;
            uid = strtol(f3 + 1, NULL, 10);
            if (uid != want) continue;

            {
                int n = 0, k;
                for (k = 0; f1[k] != '\0' && n < max - 1 && n < 32; k++) {
                    char ch = f1[k];
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                        || (ch >= '0' && ch <= '9') || ch == '.'
                        || ch == '_' || ch == '-')
                        out[n++] = ch;
                }
                out[n] = '\0';
                got = n > 0;
            }
            break;
        }
        fclose(fp);
    }

    if (!got) {
        if (max < 16)
            return -1;
        sprintf(out, "uid%ld", want);
    }
    return 0;
}

/* --- the three tiers ---------------------------------------------- */

static int g_user_redirect = -1;    /* a tier chosen for this process */
static int g_user_tier     = VLHE_USER_TIER_DEFAULT;

static const char *
user_home_path(void)
{
    static char buf[VLHE_CONF_VAL_MAX];
    const char *home = getenv("HOME");

    if (home == NULL || *home == '\0')
        return NULL;            /* a daemon started by init has none */
    if (strlen(home) + sizeof "/.vlhe/vlhe.conf" >= sizeof buf)
        return NULL;
    sprintf(buf, "%s/.vlhe/vlhe.conf", home);
    return buf;
}

/*
 * /tmp/vlhe-<uid>/ - A DIRECTORY OF OUR OWN, NOT A FILE IN /tmp.
 *
 * A 1999 /tmp is the textbook symlink target: a predictable filename
 * there is an invitation for another user to pre-create a link and
 * have this program overwrite something of ours for them. A 0700
 * directory named for the uid, checked with lstat() before use, is
 * the standard answer and costs one mkdir.
 */
static const char *
user_tmp_path(void)
{
    static char buf[VLHE_CONF_VAL_MAX];

    sprintf(buf, "/tmp/vlhe-%ld/vlhe-user.conf", (long) getuid());
    return buf;
}

static int
user_tmp_dir_ok(void)
{
    char dir[64];
    struct stat st;

    sprintf(dir, "/tmp/vlhe-%ld", (long) getuid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return -1;
    if (lstat(dir, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        errno = EACCES;         /* someone else's, or not a directory */
        return -1;
    }
    if ((st.st_mode & 077) != 0 && chmod(dir, 0700) != 0)
        return -1;
    return 0;
}

/*
 * A PRIVATE /tmp FILE, CREATED FRESH OR NOT AT ALL.
 *
 * Two callers used `/tmp/<stem>.<pid>' opened without O_EXCL - the
 * Render page's stderr capture and the Status page's run log - and
 * both run as root on these machines, where root is the normal login
 * (design/48 S6, design/49 N6). A pid is predictable, so another user
 * could leave a link at that name and have root write through it.
 *
 * O_CREAT|O_EXCL refuses any existing name INCLUDING A SYMLINK, which
 * is the property wanted, and Linux 2.2 honours it for a link. A
 * collision is not an attack by itself - a stale file from a crashed
 * run with a recycled pid looks the same - so it tries the next
 * counter rather than failing.
 */
int
vlhe_tmp_open(char *path, int max, const char *stem)
{
    int i, fd;

    if (path == NULL || max < 32 || stem == NULL)
        return -1;

    for (i = 0; i < 16; i++) {
        sprintf(path, "/tmp/%.20s.%d.%d", stem, (int) getpid(), i);
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            break;
    }
    path[0] = '\0';             /* nothing of ours to remove */
    return -1;
}

const char *
vlhe_conf_user_candidate(int tier)
{
    static char tree[VLHE_CONF_VAL_MAX];
    static char name[48];
    static char fname[80];

    switch (tier) {
    case VLHE_USER_TIER_DEFAULT:
        if (!vlhe_self_is_trial())
            return user_home_path();
        if (vlhe_conf_user_name(name, sizeof name) != 0)
            return NULL;
        sprintf(fname, "vlhe-user-%s.conf", name);
        return vlhe_self_path(fname, tree, sizeof tree) ? tree : NULL;
    case VLHE_USER_TIER_HOME:
        return user_home_path();
    case VLHE_USER_TIER_TMP:
        return user_tmp_path();
    }
    return NULL;
}

int
vlhe_conf_user_tier(void)
{
    (void) vlhe_conf_user_path();       /* resolve, so the tier is current */
    return g_user_tier;
}

int
vlhe_conf_user_redirect(int tier)
{
    if (vlhe_conf_user_candidate(tier) == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (tier == VLHE_USER_TIER_TMP && user_tmp_dir_ok() != 0)
        return -1;
    g_user_redirect = tier;
    g_user_tier     = tier;
    return 0;
}

/*
 * CAN A FILE BE CREATED HERE? Asked of the DEFAULT tier's directory
 * when its file does not exist yet - the case that decides whether
 * "the tree keeps its own" is possible at all on this filesystem.
 */
static int
dir_of_writable(const char *path)
{
    char dir[VLHE_CONF_VAL_MAX];
    const char *slash = strrchr(path, '/');

    if (slash == NULL || slash == path)
        return access("/", W_OK) == 0;
    if ((size_t) (slash - path) >= sizeof dir)
        return 0;
    memcpy(dir, path, (size_t) (slash - path));
    dir[slash - path] = '\0';
    return access(dir, W_OK) == 0;
}

const char *
vlhe_conf_user_path(void)
{
    const char *over = getenv("VLHE_USER_CONF");

    if (over != NULL && *over != '\0')
        return over;



    /*
     * TRIAL MODE KEEPS THIS IN THE FOLDER TOO - found 2026-09-22 when
     * the user asked where a reset's backups live.
     *
     * vlhe_conf_system_path() above learned about trial mode and THIS
     * DID NOT, so a trial run wrote ~/.vlhe/vlhe.conf into the user's
     * home - the one thing trial mode promises not to do. "Delete the
     * folder and every trace goes with it" has to be true of both
     * files or it is not true at all.
     *
     * THE SAME TEST, for the same reason it is duplicated there: this
     * file is a leaf that vlhe-probe links without vlhe_apply.c, so
     * it cannot call vlhe_is_trial().
     */
    /*
     * THE PORTABLE MARKER DECIDES, AND IT NO LONGER DEFERS TO AN
     * EXISTING ~/.vlhe/vlhe.conf - 2026-09-22.
     *
     * THIS BLOCK USED TO CHECK THREE THINGS, and the middle one was
     * the bug the other two were written around: whether modules were
     * installed, whether `./vsound.o' existed (against the WORKING
     * directory), and whether the user already had settings. The
     * third was added the same morning after the GUI, run out of
     * /mnt/xfer on a machine with ~/.vlhe/vlhe.conf, offered to
     * create ./vlhe-user.conf beside it - "it doesnt know what config
     * to use".
     *
     * THAT FIX WAS RIGHT FOR A PROBE AND IS WRONG FOR A MARKER. The
     * probe could be true by accident, so deferring to real settings
     * was the safe reading. A marker cannot be there by accident, and
     * the case the user named - installed VLHE, unpack a newer build
     * to test it - REQUIRES the portable copy to keep its own
     * settings. Having it silently adopt ~/.vlhe/vlhe.conf would
     * confound exactly what they were trying to measure.
     *
     * SO: a portable tree keeps its user settings in the folder,
     * always, and touches nothing in $HOME. That is what makes
     * "delete the folder and every trace goes with it" true - which
     * this function's own comment above already promised and could
     * not deliver while an existing config overrode it.
     */

        /* Unknown directory: no /proc and a bare argv[0], which means
         * PATH, which means installed. Fall through to $HOME rather
         * than return a relative path. */
    /* --- the tiers, 2026-09-24 - see vlhe_conf.h ------------------- */
    if (g_user_redirect >= 0) {
        const char *r = vlhe_conf_user_candidate(g_user_redirect);
        if (r != NULL)
            return r;
    }

    {
        const char *d = vlhe_conf_user_candidate(VLHE_USER_TIER_DEFAULT);
        const char *h = vlhe_conf_user_candidate(VLHE_USER_TIER_HOME);
        const char *t = vlhe_conf_user_candidate(VLHE_USER_TIER_TMP);
        struct stat st;

        /* THE DEFAULT WINS WHENEVER IT IS USABLE - existing, or
         * creatable. That is the 2026-09-23 decision holding. */
        if (d != NULL && (stat(d, &st) == 0 || dir_of_writable(d))) {
            g_user_tier = VLHE_USER_TIER_DEFAULT;
            return d;
        }
        /* OTHERWISE, WHERE DID A PREVIOUS SESSION PUT THEM? The same
         * order the GUI offers them in, so what was agreed to once is
         * found again without asking. */
        if (h != NULL && h != d && stat(h, &st) == 0) {
            g_user_tier = VLHE_USER_TIER_HOME;
            return h;
        }
        if (t != NULL && stat(t, &st) == 0) {
            g_user_tier = VLHE_USER_TIER_TMP;
            return t;
        }
        g_user_tier = VLHE_USER_TIER_DEFAULT;
        if (d != NULL)
            return d;
        /* NO DEFAULT AT ALL - no HOME, or no tree - and nothing saved
         * anywhere: the caller gets NULL, as before, and says so. */
        return h;
    }
}

int
vlhe_conf_writable(const char *path)
{
    char dir[VLHE_CONF_VAL_MAX];
    char *slash;

    if (path == NULL)
        return 0;

    if (access(path, W_OK) == 0)
        return 1;

    /* It may not exist yet, in which case what matters is whether we
     * could create it - so ask the directory. */
    if (errno != ENOENT)
        return 0;

    copy_bounded(dir, path, sizeof dir);

    /*
     * WALK UP TO THE NEAREST ANCESTOR THAT EXISTS, because
     * vlhe_conf_write() calls vlhe_conf_mkdir_for() and will create
     * every missing level.
     *
     * FOUND ON 86Box, 2026-09-18, by vlhe-probe run as root: it
     * reported /var/lib/vlhe/drives NOT writable on a fresh machine,
     * because /var/lib/vlhe does not exist yet and the first version
     * asked only about that one directory before giving up. Root can
     * obviously create it - and the GUI greys a setting on this
     * answer, so the drive page would have been read-only for the
     * user most able to change it.
     */
    for (;;) {
        slash = strrchr(dir, '/');
        if (slash == NULL)
            return access(".", W_OK) == 0;
        if (slash == dir)
            return access("/", W_OK) == 0;
        *slash = '\0';

        if (access(dir, F_OK) == 0)
            return access(dir, W_OK) == 0;
        /* that level is missing too - ask its parent */
    }
}

int
vlhe_conf_mkdir_for(const char *path)
{
    char dir[VLHE_CONF_VAL_MAX];
    char *p;

    copy_bounded(dir, path, sizeof dir);
    p = strrchr(dir, '/');
    if (p == NULL || p == dir)
        return 0;               /* no directory part, or the root */
    *p = '\0';

    /* Walk the components, making each. An existing one is fine; only
     * a failure that is not EEXIST stops us. */
    for (p = dir + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}
