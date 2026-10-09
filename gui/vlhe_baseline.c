/*
 * vlhe_baseline.c - the machine as it was before VLHE first changed it.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/54 7h Stage 3. vlhe_baseline.h has the reasoning.
 *
 * THE FILE, one entry per line, comments start with `;':
 *
 *     <path> | <state> | FOUND|INFERRED|UNKNOWN
 *
 * Synced like the session file (design/54 7h Stage 1.5): every write
 * is fsync()ed, and a rewrite goes through a temporary renamed over the
 * file, then the directory is synced.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include "vlhe_baseline.h"
#include "vlhe_self.h"       /* vlhe_safe_* - design/55 recommendation 2 */
#include "vlhe_session.h"

#define BASE_MAX_LINE   (2 * VLHE_PATH_MAX + 32)

static const char base_header[] =
"; vlhe-baseline - how this machine was BEFORE VLHE first changed it.\n"
";\n"
"; One line per path VLHE has touched on this machine:\n"
";\n"
";     <path> | <state> | FOUND, INFERRED or UNKNOWN\n"
";\n"
"; state     absent, link <target>, node <major> <minor> (character),\n"
";           block <major> <minor>, or other\n"
"; FOUND     read off the machine before VLHE's first change to it\n"
"; INFERRED  worked out - MAKEDEV's fixed numbers, or what an earlier\n"
";           session file recorded - because VLHE's own leftover was\n"
";           there when this line was written\n"
"; UNKNOWN   VLHE's own leftover, and the original could not be worked\n"
";           out (it is never guessed)\n"
"; DANGLING  as found, but a /dev/dsp link that may be VLHE's leftover\n"
";           and cannot be shown to be - reported until you restore the\n"
";           stock node or keep it (`vlhe repair', or the Status page)\n"
";\n"
"; WRITTEN ONCE PER PATH and never changed, except when you choose to\n"
"; update it after a Load reports a difference; the file it replaces\n"
"; is kept beside it with the time. The name carries this machine's\n"
"; hostname and root filesystem UUID, so a copy of VLHE carried to\n"
"; another machine keeps a baseline of its own there.\n"
";\n";

static void
sync_file(FILE *f)
{
    (void) fflush(f);
    (void) fsync(fileno(f));
}

static void
sync_dir_of(const char *file)
{
    char dir[VLHE_PATH_MAX];
    char *slash;
    int fd;

    strncpy(dir, file, sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == NULL)
        strcpy(dir, ".");
    else if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';
    fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        (void) fsync(fd);
        close(fd);
    }
}

/* ------------------------------------------------------------------ */
/* The machine key                                                     */
/* ------------------------------------------------------------------ */

int
vlhe_baseline_uuid_from(const char *dev, char *out, size_t max)
{
    unsigned char sb[1024];
    const unsigned char *u;
    int fd, n;

    if (out == NULL || max < 37)
        return -1;
    out[0] = '\0';
    fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    /* THE SUPERBLOCK IS 1024 BYTES IN; s_magic at 56, s_uuid at 104
     * (include/linux/ext2_fs.h). Revision 0 filesystems carry the field
     * too - Corel 1.2's installer writes one (design/54 decision 3). */
    if (lseek(fd, 1024L, SEEK_SET) != 1024L) {
        close(fd);
        return -1;
    }
    n = (int) read(fd, sb, sizeof sb);
    close(fd);
    if (n != (int) sizeof sb || sb[56] != 0x53 || sb[57] != 0xEF)
        return -1;
    u = sb + 104;
    sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                 "%02x%02x%02x%02x%02x%02x",
            u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
            u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return 0;
}

/* THE ROOT FILESYSTEM'S DEVICE NODE, found by its numbers: stat("/")
 * gives the device, and /dev is searched for the block node that names
 * it. /proc/mounts is no help on 2.2 - it says "/dev/root". */
static int
root_device(char *out, size_t max)
{
    struct stat rs, ds;
    DIR *d;
    struct dirent *e;
    char p[VLHE_PATH_MAX];

    if (stat("/", &rs) != 0)
        return -1;
    d = opendir("/dev");
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strlen(e->d_name) + 6 > sizeof p)
            continue;
        sprintf(p, "/dev/%s", e->d_name);
        if (lstat(p, &ds) == 0 && S_ISBLK(ds.st_mode)
            && ds.st_rdev == rs.st_dev && strlen(p) < max) {
            strcpy(out, p);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

const char *
vlhe_baseline_key(void)
{
    static char key[300];
    const char *env = getenv("VLHE_MACHINE_KEY");
    struct utsname u;
    char dev[VLHE_PATH_MAX], uuid[40];

    if (env != NULL && *env != '\0') {
        strncpy(key, env, sizeof key - 1);
        key[sizeof key - 1] = '\0';
        return key;
    }
    if (uname(&u) != 0)
        strcpy(u.nodename, "unknown");
    if (root_device(dev, sizeof dev) != 0
        || vlhe_baseline_uuid_from(dev, uuid, sizeof uuid) != 0)
        strcpy(uuid, "nouuid");
    sprintf(key, "%.200s/%s", u.nodename, uuid);
    return key;
}

const char *
vlhe_baseline_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *sp = vlhe_session_path();
    const char *slash = strrchr(sp, '/');
    const char *k = vlhe_baseline_key();
    size_t dl = slash != NULL ? (size_t) (slash - sp) + 1 : 0;
    size_t i, at;

    if (dl + sizeof "vlhe-baseline-" + strlen(k) > sizeof buf)
        return "vlhe-baseline";
    memcpy(buf, sp, dl);
    strcpy(buf + dl, "vlhe-baseline-");
    at = strlen(buf);
    /* FILENAME-SAFE: the key's `/' and anything odd in a hostname. */
    for (i = 0; k[i] != '\0'; i++) {
        char c = k[i];

        buf[at++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '-' || c == '.')
                    ? c : '_';
    }
    buf[at] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* States                                                              */
/* ------------------------------------------------------------------ */

void
vlhe_baseline_describe(const char *path, char *state, size_t max)
{
    struct stat sb;
    char d[VLHE_PATH_MAX + 32];

    if (state == NULL || max == 0)
        return;
    if (lstat(path, &sb) != 0)
        strcpy(d, "absent");
    else if (S_ISLNK(sb.st_mode)) {
        char t[VLHE_PATH_MAX];
        int  n = (int) readlink(path, t, sizeof t - 1);

        if (n < 0)
            n = 0;
        t[n] = '\0';
        sprintf(d, "link %.*s", VLHE_PATH_MAX - 8, t);
    } else if (S_ISCHR(sb.st_mode) || S_ISBLK(sb.st_mode)) {
        char perm[64];

        vlhe_state_perm_suffix((int) (sb.st_mode & 07777),
                               (long) sb.st_uid, (long) sb.st_gid, perm);
        sprintf(d, "%s %d %d%s", S_ISCHR(sb.st_mode) ? "node" : "block",
                major(sb.st_rdev), minor(sb.st_rdev), perm);
    } else
        strcpy(d, "other");
    strncpy(state, d, max - 1);
    state[max - 1] = '\0';
}

/*
 * THE MODE AND OWNER OF A NODE - design/55 recommendation 2, fixing R10
 * (2026-10-04). The baseline and the session recorded only "node M m",
 * so every restore remade the stock /dev/dsp as 0666 where Corel ships
 * it root:audio 0660: after one load and unload any user could open the
 * card, recording included. Recorded now, after the numbers, so every
 * reader that sscanf()s "node %d %d" still reads the numbers.
 */
void
vlhe_state_perm_suffix(int mode, long uid, long gid, char *out)
{
    sprintf(out, " mode %04o uid %ld gid %ld", mode & 07777, uid, gid);
}

/* Only a node or block state has the suffix - a link's target is a
 * path and may say anything. */
static const char *
perm_part(const char *s)
{
    if (strncmp(s, "node ", 5) != 0 && strncmp(s, "block ", 6) != 0)
        return NULL;
    return strstr(s, " mode ");
}

static size_t
state_base_len(const char *s)
{
    const char *m = perm_part(s);

    return m != NULL ? (size_t) (m - s) : strlen(s);
}

int
vlhe_state_same(const char *a, const char *b)
{
    size_t na, nb;

    if (a == NULL || b == NULL)
        return a == b;
    na = state_base_len(a);
    nb = state_base_len(b);
    return na == nb && strncmp(a, b, na) == 0;
}

int
vlhe_state_perm(const char *state, int *mode, long *uid, long *gid)
{
    const char *m;
    unsigned    md;
    long        u, g;

    if (state == NULL || (m = perm_part(state)) == NULL)
        return 0;
    if (sscanf(m, " mode %o uid %ld gid %ld", &md, &u, &g) != 3
        || md > 07777 || u < 0 || g < 0)
        return 0;
    if (mode != NULL) *mode = (int) md;
    if (uid != NULL)  *uid = u;
    if (gid != NULL)  *gid = g;
    return 1;
}

const char *
vlhe_baseline_how_word(int how)
{
    switch (how) {
    case VLHE_BASE_INFERRED: return "INFERRED";
    case VLHE_BASE_UNKNOWN:  return "UNKNOWN";
    case VLHE_BASE_DANGLING: return "DANGLING";
    default:                 return "FOUND";
    }
}

static int
how_from_word(const char *w)
{
    if (strcmp(w, "INFERRED") == 0) return VLHE_BASE_INFERRED;
    if (strcmp(w, "UNKNOWN") == 0)  return VLHE_BASE_UNKNOWN;
    if (strcmp(w, "DANGLING") == 0) return VLHE_BASE_DANGLING;
    return VLHE_BASE_FOUND;
}

/* Field `n' of "a | b | c", trimmed. */
static void
field(const char *line, int n, char *out, size_t max)
{
    const char *p = line, *e;
    size_t len;

    out[0] = '\0';
    while (n > 0 && p != NULL) {
        p = strchr(p, '|');
        if (p != NULL)
            p++;
        n--;
    }
    if (p == NULL)
        return;
    while (*p == ' ')
        p++;
    e = strchr(p, '|');
    len = e != NULL ? (size_t) (e - p) : strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\n'
                       || p[len - 1] == '\r'))
        len--;
    if (len >= max)
        len = max - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static int
parse_line(const char *line, struct vlhe_base_ent *e)
{
    char how[16];

    if (line[0] == ';' || line[0] == '\n' || line[0] == '\r'
        || strchr(line, '|') == NULL)
        return 0;
    field(line, 0, e->what, sizeof e->what);
    field(line, 1, e->state, sizeof e->state);
    field(line, 2, how, sizeof how);
    e->how = how_from_word(how);
    return e->what[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

struct vlhe_base_ent *
vlhe_baseline_read_all(int *n_out)
{
    FILE *f;
    char  line[BASE_MAX_LINE];
    struct vlhe_base_ent *rec = NULL;
    int   n = 0, cap = 0;

    *n_out = 0;
    f = vlhe_safe_state(vlhe_baseline_path());   /* judged - design/55 rec. 2 */
    if (f == NULL) {
        if (errno != ENOENT)
            *n_out = -1;
        return NULL;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (n == cap) {
            struct vlhe_base_ent *g;

            cap = cap ? cap * 2 : 16;
            g = (struct vlhe_base_ent *) realloc(rec, cap * sizeof *rec);
            if (g == NULL) {
                fclose(f);
                free(rec);
                *n_out = -1;
                return NULL;
            }
            rec = g;
        }
        if (parse_line(line, &rec[n]))
            n++;
    }
    fclose(f);
    *n_out = n;
    if (n == 0) {
        free(rec);
        return NULL;
    }
    return rec;
}

int
vlhe_baseline_get(const char *what, struct vlhe_base_ent *e)
{
    struct vlhe_base_ent *rec;
    int n, i, found = 0;

    rec = vlhe_baseline_read_all(&n);
    if (n < 0)
        return -1;
    for (i = 0; i < n; i++)
        if (strcmp(rec[i].what, what) == 0) {
            if (e != NULL)
                *e = rec[i];
            found = 1;
            break;
        }
    free(rec);
    return found;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

int
vlhe_baseline_add(const char *what, const char *state, int how)
{
    const char *path = vlhe_baseline_path();
    struct stat sb;
    int fresh, have;
    FILE *f;

    if (what == NULL || *what == '\0')
        return -1;
    have = vlhe_baseline_get(what, NULL);
    if (have < 0)
        return -1;
    if (have > 0)
        return 0;               /* written once - never again */

    fresh = (stat(path, &sb) != 0);
    f = vlhe_safe_append(path);    /* no link followed - design/55 rec. 2 */
    if (f == NULL)
        return -1;
    if (fresh)
        fputs(base_header, f);
    fprintf(f, "%s | %s | %s\n", what,
            (state != NULL && *state != '\0') ? state : "absent",
            vlhe_baseline_how_word(how));
    sync_file(f);
    if (fclose(f) != 0)
        return -1;
    if (fresh)
        sync_dir_of(path);
    return 1;
}

/* Copy `from' to `<from>.<stamp>' (-1, -2 ... when taken). 0 or -1. */
static int
keep_old(const char *from)
{
    char dest[VLHE_PATH_MAX + 40], stamp[32], buf[4096];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    struct stat sb;
    FILE *in, *out;
    size_t r;
    int k;

    if (tm != NULL)
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
    else
        strcpy(stamp, "noclock");
    if (strlen(from) + strlen(stamp) + 6 > sizeof dest)
        return -1;
    sprintf(dest, "%s.%s", from, stamp);
    for (k = 1; stat(dest, &sb) == 0 && k < 100; k++)
        sprintf(dest, "%s.%s-%d", from, stamp, k);
    if (k >= 100)
        return -1;
    in = vlhe_safe_state(from);
    if (in == NULL)
        return -1;
    out = vlhe_safe_new(dest);
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while ((r = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, r, out) != r) {
            fclose(in);
            fclose(out);
            (void) unlink(dest);
            return -1;
        }
    fclose(in);
    sync_file(out);
    if (fclose(out) != 0) {
        (void) unlink(dest);
        return -1;
    }
    return 0;
}

int
vlhe_baseline_update(const char *what, const char *state, int how)
{
    const char *path = vlhe_baseline_path();
    char tmp[VLHE_PATH_MAX + 8], line[BASE_MAX_LINE];
    FILE *in, *out;
    int done = 0;

    if (what == NULL || *what == '\0')
        return -1;
    if (vlhe_baseline_get(what, NULL) <= 0)
        return vlhe_baseline_add(what, state, how) > 0 ? 0 : -1;

    /* NEVER UPDATED WITHOUT KEEPING WHAT IT WAS - 7h Stage 3. */
    if (keep_old(path) != 0)
        return -1;

    if (strlen(path) + 5 > sizeof tmp)
        return -1;
    sprintf(tmp, "%s.tmp", path);
    in = vlhe_safe_state(path);
    if (in == NULL)
        return -1;
    out = vlhe_safe_new(tmp);
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while (fgets(line, sizeof line, in) != NULL) {
        struct vlhe_base_ent e;

        if (!done && parse_line(line, &e) && strcmp(e.what, what) == 0) {
            fprintf(out, "%s | %s | %s\n", what,
                    (state != NULL && *state != '\0') ? state : "absent",
                    vlhe_baseline_how_word(how));
            done = 1;
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    sync_file(out);
    if (fclose(out) != 0 || rename(tmp, path) != 0) {
        (void) unlink(tmp);
        return -1;
    }
    sync_dir_of(path);
    return 0;
}
