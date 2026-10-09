/*
 * vlhe_session.c - the load/unload session file.
 * READ vlhe_session.h FIRST, and design/40-session-state.md before
 * that. This is the mechanism; they have the why.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>          /* open(O_RDONLY), for syncing a directory */
#include <sys/stat.h>
#include <sys/types.h>

#include "vlhe_session.h"
#include "vlhe_self.h"
#include "vlhe_status.h"

#define SESS_MAX_LINE   512

/* ------------------------------------------------------------------ */
/* Where                                                              */
/* ------------------------------------------------------------------ */

/*
 * THE SAME LADDER vlhe_dsp_saved_path() CLIMBS, and deliberately so:
 * an override wins, a portable copy keeps its state beside its own
 * binary because a copy that promises to touch nothing of the system
 * must not write to /var, and otherwise /var/lib/vlhe beside
 * `drives'.
 */
/*
 * NO POINTER BETWEEN THE BUILDS - removed 2026-10-04 (design/55 13f, the
 * user: "each build cleans up after itself"). From 2026-10-02 a portable
 * session write left its file's path in <rundir>/session and an INSTALLED
 * vlhe_session_path() followed it, so the installed copy's `dpkg -r' could
 * undo a portable load (tests/logs/2026-10-02-86box-deb-install-and-dpkg-r/).
 * That was root acting on a file in a folder - design/55 R3's shape - and
 * the last hybrid the two builds left. Now each build reads only its own
 * session file. An installed unload after a portable load still stops the
 * daemons (pid files) and removes the modules (by name), and a /dev/dsp left
 * pointing at vsound is what the Status page's Needs attention offers to
 * put right.
 */
const char *
vlhe_session_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *env = getenv("VLHE_SESSION");

    if (env != NULL && *env != '\0') {
        strncpy(buf, env, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        return buf;
    }

    env = getenv("VLHE_MODULE_DIR");
    if (env != NULL && *env != '\0'
        && strlen(env) + sizeof "/vlhe-session" <= sizeof buf) {
        strcpy(buf, env);
        strcat(buf, "/vlhe-session");
        return buf;
    }

    if (vlhe_self_is_trial()
        && vlhe_self_path("vlhe-session", buf, sizeof buf))
        return buf;

    strcpy(buf, "/var/lib/vlhe/vlhe-session");
    return buf;
}

/*
 * THE COMPLETED ONES, IN A DIRECTORY OF THEIR OWN - `sessions'
 * beside the live file.
 *
 * THE LIVE FILE STAYS OUT OF IT ON PURPOSE. "Is anything
 * outstanding?" is then one stat() of a known path rather than
 * listing a directory and reading every file in it.
 */
const char *
vlhe_session_dir(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *live = vlhe_session_path();
    char *slash;

    strncpy(buf, live, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    slash = strrchr(buf, '/');
    if (slash == NULL) {
        strcpy(buf, "sessions");
        return buf;
    }
    slash[1] = '\0';
    if (strlen(buf) + sizeof "sessions" <= sizeof buf)
        strcat(buf, "sessions");
    return buf;
}

/* ------------------------------------------------------------------ */
/* The file format                                                    */
/* ------------------------------------------------------------------ */

/*
 * COMMENTED LIKE `vlhe.conf', which is the user's call, 2026-09-25:
 * "commented the same way the configs are so someone can understand
 * what was changed what didnt change and what failed".
 *
 * AN EARLIER DRAFT OF design/40 CALLED THIS FILE MACHINE-ONLY and
 * that was wrong - it contradicted the same document's claim that an
 * unmatched entry must be VISIBLE rather than inferred from a device
 * listing. Something visible has a reader. The distinction that
 * matters is that the JOURNAL is written for an auditor and this is
 * written for the undo; being legible on the way costs nothing.
 */
static const char *const sess_header =
    "; vlhe-session - what this load changed, and how the unload went.\n"
    ";\n"
    "; GENERATED, and read by `vlhe apply -u'. One record per line:\n"
    ";\n"
    ";     <kind> <what> | <prior state> | <state> | <note>\n"
    ";\n"
    "; state   OPEN    the change is in effect\n"
    ";         UNDONE  reversed - the machine no longer carries it\n"
    ";         FAILED  the undo could not; the next unload retries it\n"
    ";         CONFLICT  someone else changed it while VLHE was loaded -\n"
    ";                 left as found, and final\n"
    ";\n"
    "; WHEN EVERY LINE READS UNDONE OR CONFLICT this file is finished and\n"
    "; is moved to `sessions/' with the time it completed. One that still\n"
    "; has an OPEN or FAILED line stays here, because the machine is still\n"
    "; carrying something and the next unload must find it.\n"
    ";\n"
    "; The files under `sessions/' are kept so a past load can be read\n"
    "; back, and the baseline reads them when it is first recorded, to\n"
    "; tell a link VLHE left behind from the machine's own.\n"
    "; DELETING THEM IS SAFE - it loses that history and breaks nothing.\n"
    ";\n";

static const char *
kind_word(int k)
{
    switch (k) {
    case VLHE_SESS_NODE:   return "node";
    case VLHE_SESS_LINK:   return "link";
    case VLHE_SESS_MODULE: return "module";
    case VLHE_SESS_DAEMON: return "daemon";
    default:               return "?";
    }
}

static int
word_kind(const char *w)
{
    if (strcmp(w, "node") == 0)   return VLHE_SESS_NODE;
    if (strcmp(w, "link") == 0)   return VLHE_SESS_LINK;
    if (strcmp(w, "module") == 0) return VLHE_SESS_MODULE;
    if (strcmp(w, "daemon") == 0) return VLHE_SESS_DAEMON;
    return -1;
}

static const char *
state_word(int s)
{
    switch (s) {
    case VLHE_SESS_UNDONE: return "UNDONE";
    case VLHE_SESS_FAILED: return "FAILED";
    case VLHE_SESS_CONFLICT: return "CONFLICT";
    default:               return "OPEN";
    }
}

static int
word_state(const char *w)
{
    if (strcmp(w, "UNDONE") == 0) return VLHE_SESS_UNDONE;
    if (strcmp(w, "FAILED") == 0) return VLHE_SESS_FAILED;
    if (strcmp(w, "CONFLICT") == 0) return VLHE_SESS_CONFLICT;
    return VLHE_SESS_OPEN;
}

/*
 * `|' SEPARATES THE FIELDS, AND NOT WHITESPACE.
 *
 * A path can contain spaces and a note certainly can - "no such
 * process", an argv. Splitting on whitespace would put the second
 * half of a note in the wrong column and, worse, would do it
 * silently. The bar cannot appear in a device path, and the one
 * place it could appear - a note we compose ourselves - is ours to
 * keep clean.
 */
static void
field(const char *line, int n, char *out, size_t max)
{
    const char *p = line;
    const char *end;
    size_t len;
    int i;

    out[0] = '\0';
    for (i = 0; i < n; i++) {
        p = strchr(p, '|');
        if (p == NULL)
            return;
        p++;
    }
    end = strchr(p, '|');
    len = (end != NULL) ? (size_t)(end - p) : strlen(p);

    while (len > 0 && (*p == ' ' || *p == '\t')) {
        p++;
        len--;
    }
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'
                       || p[len - 1] == '\n' || p[len - 1] == '\r'))
        len--;

    if (len > max - 1)
        len = max - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

/*
 * ON THE DISK, NOT IN THE BUFFER CACHE - design/54 7h Stage 1.5,
 * 2026-10-03. ext2 on 2.2 has no journal, and its buffers are flushed
 * on a 5-30 s timer (CLAUDE.md section 2: a hard-reset machine loses
 * everything after the last flush). The session file is what an unload
 * after a power loss reads, so every write to it is fsync()ed - and the
 * DIRECTORY too after a rename, since the new name is a directory
 * change; 2.2's ext2 honours fsync() on a directory (fs/ext2/dir.c uses
 * ext2_sync_file). Failures are ignored: the write itself succeeded.
 */
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

int
vlhe_session_add(int kind, const char *what, const char *prior,
                 const char *note)
{
    const char *path = vlhe_session_path();
    struct stat sb;
    int   fresh;
    FILE *f;

    if (what == NULL || *what == '\0')
        return -1;

    fresh = (stat(path, &sb) != 0);

    /* THE DIRECTORY MAY NOT EXIST on a machine that has never been
     * loaded. Ignore the result: if it was already there mkdir fails
     * harmlessly, and if it could not be made the fopen below says
     * so with a better error than we could. */
    if (fresh) {
        char dir[VLHE_PATH_MAX];
        char *slash;

        strncpy(dir, path, sizeof dir - 1);
        dir[sizeof dir - 1] = '\0';
        slash = strrchr(dir, '/');
        if (slash != NULL && slash != dir) {
            *slash = '\0';
            (void) mkdir(dir, 0755);
        }
    }

    f = vlhe_safe_append(path);    /* no link followed - design/55 rec. 2 */
    if (f == NULL)
        return -1;

    if (fresh)
        fputs(sess_header, f);

    /* A NEW LOAD APPENDS rather than starting over: a FAILED line
     * from a previous session is still outstanding state, and a
     * fresh file would lose the only record that something was left
     * behind. */
    fprintf(f, "%s %s | %s | %s | %s\n",
            kind_word(kind), what,
            (prior != NULL && *prior != '\0') ? prior : "-",
            state_word(VLHE_SESS_OPEN),
            (note != NULL && *note != '\0') ? note : "-");

    sync_file(f);
    if (fclose(f) != 0)
        return -1;
    if (fresh)
        sync_dir_of(path);      /* a new file is a new name */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

/* ONE LINE INTO ONE RECORD - 1 when it is a record, 0 for a comment,
 * a blank, or a line from a newer version. Shared by both readers so
 * they cannot disagree about what a record is. */
static int
parse_line(char *line, struct vlhe_sess_rec *r)
{
    char kw[32];
    char sw[32];
    char first[VLHE_PATH_MAX];
    char *sp;

    if (line[0] == ';' || line[0] == '\n' || line[0] == '\r')
        return 0;

    field(line, 0, first, sizeof first);
    if (first[0] == '\0')
        return 0;

    /* The first field is "<kind> <what>", split at the FIRST
     * space - a kind word never contains one and a path may. */
    sp = strchr(first, ' ');
    if (sp == NULL)
        return 0;
    *sp = '\0';
    strncpy(kw, first, sizeof kw - 1);
    kw[sizeof kw - 1] = '\0';

    memset(r, 0, sizeof *r);
    r->kind = word_kind(kw);
    if (r->kind < 0)
        return 0;               /* a line from a newer version */

    strncpy(r->what, sp + 1, sizeof r->what - 1);
    field(line, 1, r->prior, sizeof r->prior);
    field(line, 2, sw, sizeof sw);
    r->state = word_state(sw);
    field(line, 3, r->note, sizeof r->note);

    if (strcmp(r->prior, "-") == 0)
        r->prior[0] = '\0';
    if (strcmp(r->note, "-") == 0)
        r->note[0] = '\0';
    return 1;
}

int
vlhe_session_read(struct vlhe_sess_rec *out, int max)
{
    const char *path = vlhe_session_path();
    char  line[SESS_MAX_LINE];
    FILE *f;
    int   n = 0;

    if (out == NULL || max <= 0)
        return -1;

    f = vlhe_safe_state(path);     /* judged first - design/55 rec. 2 */
    if (f == NULL) {
        /* NOTHING RECORDED IS NOT THE SAME AS CANNOT LOOK, and the
         * caller has to be able to tell them apart - conflating them
         * is what let six unloads in a row report success while
         * leaving a symlink behind. ENOENT is 0; anything else is an
         * error worth reporting. */
        return (errno == ENOENT) ? 0 : -1;
    }

    while (n < max && fgets(line, sizeof line, f) != NULL)
        if (parse_line(line, &out[n]))
            n++;

    fclose(f);
    return n;
}

int
vlhe_session_note(const char *text)
{
    FILE *f;

    if (text == NULL)
        return -1;
    f = vlhe_safe_append(vlhe_session_path());
    if (f == NULL)
        return -1;
    fprintf(f, "; %s\n", text);
    sync_file(f);
    return fclose(f) == 0 ? 0 : -1;
}

struct vlhe_sess_rec *
vlhe_session_read_all(int *n_out)
{
    return vlhe_session_read_path(vlhe_session_path(), n_out);
}

/* ANY SESSION FILE, not only the live one - the baseline's inference
 * reads the filed ones in sessions/ too (design/54 7h Stage 3: "the
 * SESSION FILES record (live or filed)"). */
struct vlhe_sess_rec *
vlhe_session_read_path(const char *path, int *n_out)
{
    char  line[SESS_MAX_LINE];
    struct vlhe_sess_rec *rec = NULL, *grown;
    int   n = 0, cap = 0;
    FILE *f;

    *n_out = 0;
    f = vlhe_safe_state(path);
    if (f == NULL) {
        /* As vlhe_session_read(): absent is 0, unreadable is -1 - and
         * a file refused by vlhe_safe_state() is unreadable. */
        *n_out = (errno == ENOENT) ? 0 : -1;
        return NULL;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            grown = (struct vlhe_sess_rec *)
                    realloc(rec, (size_t) cap * sizeof *rec);
            if (grown == NULL) {
                /* OUT OF MEMORY IS "CANNOT READ", never a short list
                 * - a short list is exactly the D22 failure. */
                free(rec);
                fclose(f);
                *n_out = -1;
                return NULL;
            }
            rec = grown;
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

/* ------------------------------------------------------------------ */
/* Marking                                                            */
/* ------------------------------------------------------------------ */

/*
 * A MARK KEEPS WHAT THE LOAD NOTED - 86Box 2026-10-03. The note is
 * "; "-separated facts the Load wrote ("was node 14 3", "card at
 * /dev/dsp0", "made /dev/dsp1") that later steps parse, and a mark used
 * to REPLACE it with its reason: /dev/dsp's FAILED "could not restore"
 * wiped `made', so the retry fell back to the looser link test, and the
 * filed record still read "could not restore" after the retry worked.
 *
 * So: the facts are kept in order (remove_recorded_nodes() reads "was b"
 * from the START); a FAILED reason is its own "failed: <why>" segment;
 * every mark drops the previous one, so a retry that works clears it and
 * a second failure replaces it. Other reasons ("already gone", the
 * CONFLICT's) are appended plain. MARKING OPEN WITH A NOTE STILL
 * REPLACES IT - session_link_open() builds the whole note itself.
 */
static void
note_merge(char *out, size_t max, const char *old, int state,
           const char *note)
{
    const char *p = old;
    size_t      len = 0;

    out[0] = '\0';
    if (state == VLHE_SESS_OPEN && note != NULL && *note != '\0') {
        strncpy(out, note, max - 1);
        out[max - 1] = '\0';
        return;
    }
    while (p != NULL && *p != '\0') {
        const char *e = strchr(p, ';');
        size_t      n;

        while (*p == ' ')
            p++;
        n = e != NULL ? (size_t) (e - p) : strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\n'))
            n--;
        if (n > 0 && !(n == 1 && *p == '-')
            && strncmp(p, "failed: ", 8) != 0
            && len + n + 3 < max) {
            if (len > 0) {
                memcpy(out + len, "; ", 2);
                len += 2;
            }
            memcpy(out + len, p, n);
            len += n;
            out[len] = '\0';
        }
        p = e != NULL ? e + 1 : NULL;
    }
    if (note != NULL && *note != '\0') {
        const char *tag = state == VLHE_SESS_FAILED ? "failed: " : "";
        size_t      need = (len > 0 ? 2 : 0) + strlen(tag) + strlen(note);

        if (len + need < max) {
            sprintf(out + len, "%s%s%s", len > 0 ? "; " : "", tag, note);
            len += need;
        }
    }
    if (len == 0)
        strcpy(out, "-");
}

/*
 * REWRITTEN THROUGH A TEMPORARY AND RENAMED OVER, so an interrupted
 * mark cannot leave a half-written undo list - which would be worse
 * than no list at all, because the machine's outstanding state would
 * then be unreadable rather than merely unknown.
 */
int
vlhe_session_mark(int i, int state, const char *note)
{
    const char *path = vlhe_session_path();
    char  tmp[VLHE_PATH_MAX];
    char  line[SESS_MAX_LINE];
    FILE *in, *out;
    int   n = 0;

    if (i < 0)
        return -1;
    if (strlen(path) + sizeof ".tmp" > sizeof tmp)
        return -1;
    strcpy(tmp, path);
    strcat(tmp, ".tmp");

    in = vlhe_safe_state(path);
    if (in == NULL)
        return -1;
    out = vlhe_safe_new(tmp);      /* no link followed - design/55 rec. 2 */
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    while (fgets(line, sizeof line, in) != NULL) {
        if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') {
            fputs(line, out);
            continue;
        }
        if (n == i) {
            char head[VLHE_PATH_MAX];
            char prior[VLHE_PATH_MAX];
            char old[SESS_MAX_LINE];
            char merged[SESS_MAX_LINE];

            /* FIELD 0 IS "<kind> <what>" TOGETHER and is written
             * back verbatim, which is why the kind survives a mark
             * without this function knowing what a kind is. */
            field(line, 0, head,  sizeof head);
            field(line, 1, prior, sizeof prior);
            field(line, 3, old,   sizeof old);
            note_merge(merged, sizeof merged, old, state, note);

            fprintf(out, "%s | %s | %s | %s\n",
                    head,
                    prior[0] != '\0' ? prior : "-",
                    state_word(state),
                    merged);
        } else {
            fputs(line, out);
        }
        n++;
    }

    fclose(in);
    sync_file(out);
    if (fclose(out) != 0) {
        (void) unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        (void) unlink(tmp);
        return -1;
    }
    sync_dir_of(path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rotating                                                           */
/* ------------------------------------------------------------------ */

/*
 * THE FOLDER SAYS WHAT IT IS - design/40 4f, the user's decision of
 * 2026-09-25 ("let the user know we store logs there kind of thing and
 * that it is safe to delete them"), the second of the two places it
 * named; design/54 G17. Written the first time a session is filed,
 * and never over an existing README - one the user edited is theirs.
 * A failure is not reported: the folder works without it.
 */
static void
sessions_readme(const char *dir)
{
    char  path[VLHE_PATH_MAX];
    FILE *f;
    struct stat sb;

    if (strlen(dir) + sizeof "/README" > sizeof path)
        return;
    strcpy(path, dir);
    strcat(path, "/README");
    if (stat(path, &sb) == 0)
        return;
    f = vlhe_safe_new(path);
    if (f == NULL)
        return;
    fputs("VLHE - completed sessions\n"
          "\n"
          "Each file here is one load and unload of VLHE, named by when\n"
          "the unload finished (YYYYMMDD-HHMMSS). It lists what the load\n"
          "changed - modules, daemons, device nodes, the /dev/dsp and\n"
          "/dev/cdrom links - and that the unload put each one back.\n"
          "\n"
          "They are kept so that a past session can be looked at again,\n"
          "and the baseline reads them when it is first recorded, to tell\n"
          "a link VLHE left behind from the machine's own. Nothing\n"
          "removes them.\n"
          "\n"
          "Deleting them is safe: it loses that history and breaks\n"
          "nothing.\n"
          "\n"
          "A session that did NOT finish cleanly is never moved here. It\n"
          "stays as vlhe-session beside this folder, so the next unload\n"
          "can find it and finish putting the machine back.\n", f);
    (void) fclose(f);
}

int
vlhe_session_rotate(void)
{
    struct vlhe_sess_rec *rec;
    const char *path = vlhe_session_path();
    const char *dir  = vlhe_session_dir();
    char   dest[VLHE_PATH_MAX];
    char   stamp[32];
    time_t now;
    struct tm *tm;
    int    n, i;

    rec = vlhe_session_read_all(&n);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;               /* nothing recorded; nothing to move */

    /* EVERY LINE MUST READ UNDONE. One OPEN means the load is still
     * up; one FAILED means the machine is still carrying something
     * and the next unload has to find this file. EVERY line, not the
     * first 64 (design/54 D22). */
    for (i = 0; i < n; i++)
        if (rec[i].state != VLHE_SESS_UNDONE
            && rec[i].state != VLHE_SESS_CONFLICT) {   /* final - 7h */
            free(rec);
            return 0;
        }
    free(rec);

    (void) mkdir(dir, 0755);
    sessions_readme(dir);

    /* time() AND localtime(), NOT A `date' SUBPROCESS - this runs
     * inside a program, and the shape matches load.sh's run-<stamp>
     * so the two are comparable by eye. */
    now = time(NULL);
    tm  = localtime(&now);
    if (tm == NULL)
        return -1;
    sprintf(stamp, "%04d%02d%02d-%02d%02d%02d",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);

    if (strlen(dir) + 1 + strlen(stamp) + 4 > sizeof dest)
        return -1;
    strcpy(dest, dir);
    strcat(dest, "/");
    strcat(dest, stamp);

    /*
     * A NAME ALREADY TAKEN GETS A SUFFIX - design/54 7h Stage 1.4,
     * 2026-10-03. Two sessions filed in the same second have the same
     * stamp, and rename() REPLACES an existing file without a word: the
     * first session's record was lost (found writing test_vlhe_session).
     * -1, -2 ... up to -99, then refuse rather than overwrite - the
     * live file stays, which only means it is filed next time.
     */
    {
        struct stat sb;
        size_t base = strlen(dest);
        int k;

        for (k = 1; stat(dest, &sb) == 0; k++) {
            if (k > 99)
                return -1;
            sprintf(dest + base, "-%d", k);
        }
    }

    if (rename(path, dest) != 0)
        return -1;
    /* THE MOVE CHANGES TWO DIRECTORIES - the live one loses the name,
     * sessions/ gains it - so both are synced (the other agent's point,
     * 7h Stage 1.5). */
    sync_dir_of(path);
    sync_dir_of(dest);
    return 1;
}
