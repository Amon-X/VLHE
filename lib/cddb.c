/*
 * cddb.c - the disc id, and reading what KsCD and grip have cached.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * READ cddb.h FIRST. It has where the records live, why we read
 * other players' caches rather than talking to a server, and why
 * CD-TEXT wins where both exist.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cddb.h"

/* ------------------------------------------------------------------ */
/* The disc id                                                        */
/* ------------------------------------------------------------------ */

/*
 * SUM THE DECIMAL DIGITS of a number. workman's `cddb_sum', carried
 * into KsCD unchanged (`cdrom.c:104') under its own instruction that
 * the algorithm must not change.
 *
 * IT IS THE DIGITS OF THE DECIMAL SPELLING, not the value - 125
 * contributes 1+2+5 = 8. Reading it as anything else gives an id
 * that looks right and matches no record anywhere.
 */
static int
cddb_sum(int n)
{
    char buf[16];
    char *p;
    int   ret = 0;

    sprintf(buf, "%lu", (unsigned long) n);
    for (p = buf; *p != '\0'; p++)
        ret += (*p - '0');
    return ret;
}

unsigned long
vlhe_cddb_discid(const int *starts, int ntracks, int leadout)
{
    int i, t, n = 0;

    if (starts == NULL || ntracks <= 0)
        return 0;

    /*
     * THE +150 IS THE LEAD-IN, and it is the easiest thing here to
     * get wrong. CDDB works in ABSOLUTE MSF, where the first sector
     * of a disc is 00:02:00 - 150 sectors in. This project holds
     * FILE-RELATIVE LBAs with no lead-in (CLAUDE.md section 5: "the
     * driver adds the +150 MSF offset; the layer strips it"), so it
     * is added back here.
     *
     * Without it every seconds value is two low, the digit sums
     * differ, and the id is plausible and wrong.
     */
    for (i = 0; i < ntracks; i++)
        n += cddb_sum((starts[i] + 150) / 75);

    t = (leadout + 150) / 75 - (starts[0] + 150) / 75;

    return ((unsigned long) (n % 0xff) << 24)
         | ((unsigned long) t << 8)
         | (unsigned long) ntracks;
}

/* ------------------------------------------------------------------ */
/* The xmcd record                                                    */
/* ------------------------------------------------------------------ */

/*
 * COPY A FIELD, BOUNDED, STRIPPING THE TRAILING NEWLINE.
 *
 * The records are files other programs wrote and a user may have
 * edited by hand, so nothing about their length is guaranteed.
 */
static void
field_copy(char *dst, size_t max, const char *src)
{
    size_t n = 0;

    while (src[n] != '\0' && src[n] != '\n' && src[n] != '\r'
           && n + 1 < max) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

int
vlhe_cddb_parse_file(const char *path, struct cdtext *out)
{
    FILE *fp;
    char  line[512];
    int   got = 0;

    if (path == NULL || out == NULL)
        return -1;

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;

    memset(out, 0, sizeof *out);

    while (fgets(line, sizeof line, fp) != NULL) {
        /* COMMENTS CARRY THE TRACK OFFSETS AND THE DISC LENGTH, which
         * we already know from the TOC - skipped rather than parsed. */
        if (line[0] == '#')
            continue;

        if (strncmp(line, "DISCID=", 7) == 0) {
            got = 1;
            continue;
        }

        if (strncmp(line, "DTITLE=", 7) == 0) {
            /*
             * "artist / title" IN ONE FIELD, and the separator is
             * " / " with spaces - grip writes it that way
             * (`cddb.c:708') and so does every server.
             *
             * A TITLE MAY CONTAIN A SLASH, so the FIRST " / " is the
             * split and the rest is the title. Splitting on the last
             * would mangle "AC/DC / Back in Black" the other way;
             * neither is right for every disc and this is what the
             * writers intend.
             *
             * NO SEPARATOR AT ALL means the whole line is the title
             * - a self-titled disc, or a hand-edited file.
             */
            char *sep = strstr(line + 7, " / ");

            if (sep != NULL) {
                size_t alen = (size_t) (sep - (line + 7));

                if (alen >= sizeof out->disc.performer)
                    alen = sizeof out->disc.performer - 1;
                memcpy(out->disc.performer, line + 7, alen);
                out->disc.performer[alen] = '\0';
                field_copy(out->disc.title, sizeof out->disc.title,
                           sep + 3);
            } else {
                field_copy(out->disc.title, sizeof out->disc.title,
                           line + 7);
            }
            got = 1;
            continue;
        }

        if (strncmp(line, "TTITLE", 6) == 0) {
            int   t = atoi(line + 6);
            char *eq = strchr(line, '=');

            /*
             * TRACKS ARE 0-BASED IN THE FILE AND 1-BASED IN cdtext,
             * which is the disc's own numbering and what every other
             * part of this project uses.
             */
            if (eq != NULL && t >= 0 && t + 1 < CDTEXT_MAX_TRACKS)
                field_copy(out->track[t + 1].title,
                           sizeof out->track[t + 1].title, eq + 1);
            continue;
        }

        /*
         * EVERYTHING ELSE IS IGNORED, DELIBERATELY: EXTD, EXTTn,
         * PLAYORDER, and grip's non-standard DYEAR and DGENRE
         * (`cddb.c:712'). A record from a newer writer must not be
         * refused for carrying a key we do not know.
         */
    }

    fclose(fp);

    if (!got)
        return -1;                      /* a file, but not a record */

    out->present = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Finding one                                                        */
/* ------------------------------------------------------------------ */

/*
 * THE GENRES, in CDDB's own order. Both players walk this list when
 * the flat path misses; `CDDBGenre()' in grip (`cddb.c:73') and the
 * directories Corel ships with KsCD are the same twelve.
 */
static const char *const genre[] = {
    "blues", "classical", "country", "data", "folk", "jazz",
    "misc", "newage", "reggae", "rock", "soundtrack"
};
#define NGENRE ((int) (sizeof genre / sizeof genre[0]))

/*
 * THE BASE DIRECTORIES, in the order they are tried.
 *
 * THE CALLER'S HOME FIRST, because a record they fetched themselves
 * is the most likely to be the one they want; then root's, because
 * the GUI may run as either and a record fetched by one user is
 * invisible to the other; then KsCD's system-wide directory, which
 * Corel ships.
 *
 * `~/.cddb' IS grip's (`cddb.c:532') and the system path is KsCD's
 * (`kscdrc''s LocalBaseDir, default
 * /usr/X11R6/share/apps/kscd/cddb/).
 */
static int
base_dirs(char out[4][256])
{
    const char *home = getenv("HOME");
    int n = 0;

    if (home != NULL && *home != '\0') {
        sprintf(out[n], "%.240s/.cddb", home);
        n++;
    }
    /* ROOT'S, when we are not root ourselves - and skipped when the
     * caller's home IS /root, so the same path is not searched twice. */
    if (home == NULL || strcmp(home, "/root") != 0) {
        strcpy(out[n], "/root/.cddb");
        n++;
    }
    strcpy(out[n], "/usr/X11R6/share/apps/kscd/cddb");
    n++;
    return n;
}

static int
try_path(const char *path, struct cdtext *out)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    return vlhe_cddb_parse_file(path, out);
}

int
vlhe_cddb_lookup(unsigned long discid, struct cdtext *out)
{
    char        dirs[4][256];
    char        path[512];
    const char *env;
    int         ndirs, i, g;

    if (out == NULL)
        return -1;

    /*
     * VLHE_CDDB_DIRS REPLACES the search path, for the host test.
     * Colon-separated, like PATH. Safe to exercise here: this file
     * opens no device and writes nothing anywhere.
     */
    env = getenv("VLHE_CDDB_DIRS");
    if (env != NULL && *env != '\0') {
        char  buf[1024];
        char *p, *q;

        strncpy(buf, env, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        ndirs = 0;
        for (p = buf; p != NULL && ndirs < 4; p = q) {
            q = strchr(p, ':');
            if (q != NULL)
                *q++ = '\0';
            if (*p != '\0') {
                /* sprintf WITH A WIDTH rather than strncpy: the host
                 * gcc warns that a 1023-byte source may be truncated
                 * into 255, which is exactly what is intended, and a
                 * bounded sprintf says so without the warning. */
                sprintf(dirs[ndirs], "%.255s", p);
                ndirs++;
            }
        }
    } else {
        ndirs = base_dirs(dirs);
    }

    for (i = 0; i < ndirs; i++) {
        /* FLAT FIRST, as grip does (`cddb.c:591') - KsCD only writes
         * the genre form, but a hand-placed record is usually flat. */
        sprintf(path, "%.240s/%08lx", dirs[i], discid);
        if (try_path(path, out) == 0)
            return 0;

        for (g = 0; g < NGENRE; g++) {
            sprintf(path, "%.200s/%s/%08lx", dirs[i], genre[g], discid);
            if (try_path(path, out) == 0)
                return 0;
        }
    }

    return -1;
}
