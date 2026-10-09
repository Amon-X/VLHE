/*
 * vdiscd_state.c - the drive state file. vdiscd_state.h has what and
 * why; this is the reading and the writing.
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
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "vdiscd_state.h"

void
vdiscd_state_clear(struct vdiscd_state *st)
{
    memset(st, 0, sizeof *st);
    st->cdrom = -1;
}

int
vdiscd_state_cdrom(const struct vdiscd_state *st)
{
    return vdiscd_state_cdrom_in(st, VDISC_MAX_DEVS);
}

int
vdiscd_state_cdrom_in(const struct vdiscd_state *st, int ndrives)
{
    if (ndrives <= 0 || ndrives > VDISC_MAX_DEVS)
        ndrives = VDISC_MAX_DEVS;
    if (st == NULL || st->cdrom < 0 || st->cdrom >= ndrives)
        return 0;
    return st->cdrom;
}

int
vdiscd_state_wanted(const struct vdiscd_state *st, int drive)
{
    if (st == NULL || drive < 0 || drive >= VDISC_MAX_DEVS)
        return 0;
    return st->autoload[drive] && st->path[drive][0] != '\0';
}

/* `Name<digits> = value' - the drive number out of the key, the value
 * trimmed. 1 if the line had that shape, else 0. */
static int
key_line(char *line, const char *name, int *drive, char **value)
{
    size_t nl = strlen(name);
    char  *p, *end;
    long   d;

    p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, name, nl) != 0)
        return 0;
    p += nl;
    if (*p < '0' || *p > '9')
        return 0;
    d = strtol(p, &end, 10);
    p = end;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    end = p + strlen(p);
    while (end > p && (end[-1] == '\n' || end[-1] == '\r'
                       || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    if (d < 0 || d >= VDISC_MAX_DEVS)
        return 0;
    *drive = (int) d;
    *value = p;
    return 1;
}

int
vdiscd_state_read(const char *file, struct vdiscd_state *st)
{
    FILE *fp;
    char  line[VDISCD_STATE_PATH + 64];

    vdiscd_state_clear(st);
    if (file == NULL || *file == '\0')
        return -1;
    fp = fopen(file, "r");
    if (fp == NULL)
        return -1;
    while (fgets(line, sizeof line, fp) != NULL) {
        int   d;
        char *v;

        /* A LINE LONGER THAN THE BUFFER is not ours to half-read: skip
         * the rest of it, and the fragment is not a key. */
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            int c;

            while ((c = fgetc(fp)) != EOF && c != '\n')
                ;
            continue;
        }
        if (key_line(line, "Drive", &d, &v)) {
            strncpy(st->path[d], v, VDISCD_STATE_PATH - 1);
            st->path[d][VDISCD_STATE_PATH - 1] = '\0';
        } else if (key_line(line, "Autoload", &d, &v)) {
            st->autoload[d] = (atoi(v) != 0);
        } else {
            /* `Cdrom = N' - no digits in the key, so key_line() does
             * not apply; the value is the drive. */
            char *p = line;

            while (*p == ' ' || *p == '\t')
                p++;
            if (strncmp(p, "Cdrom", 5) == 0) {
                p += 5;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == '=') {
                    int n = atoi(p + 1);

                    if (n >= 0 && n < VDISC_MAX_DEVS)
                        st->cdrom = n;
                }
            }
        }
    }
    fclose(fp);
    return 0;
}

int
vdiscd_state_write(const char *file, const struct vdiscd_state *st)
{
    char  tmp[VDISCD_STATE_PATH + 16];
    FILE *fp;
    int   d, bad;

    if (file == NULL || *file == '\0' || strlen(file) + 5 > sizeof tmp) {
        errno = EINVAL;
        return -1;
    }
    sprintf(tmp, "%s.new", file);
    fp = fopen(tmp, "w");
    if (fp == NULL)
        return -1;

    fprintf(fp, "; %s - what is in each virtual drive, and whether it\n"
                "; comes back at the next load (\"Load at startup\").\n"
                "; WRITTEN BY vdiscd on every attach, eject and change of\n"
                "; that box; an edit made while VLHE is loaded is lost.\n"
                "; Nothing comes back unless [CD Settings] DrivesAutoLoad\n"
                "; = 1 in vlhe.conf.\n"
                "[Drives]\n", file);
    for (d = 0; d < VDISC_MAX_DEVS; d++) {
        /* A NEWLINE IN A PATH would end the value and start a line of
         * its own - recorded as empty rather than as two lines. */
        if (st->path[d][0] != '\0' && strchr(st->path[d], '\n') == NULL
            && strchr(st->path[d], '\r') == NULL)
            fprintf(fp, "Drive%d = %s\n", d, st->path[d]);
        if (st->autoload[d])
            fprintf(fp, "Autoload%d = 1\n", d);
    }
    if (st->cdrom >= 0 && st->cdrom < VDISC_MAX_DEVS)
        fprintf(fp, "Cdrom = %d\n", st->cdrom);

    bad = ferror(fp);
    if (fclose(fp) != 0)
        bad = 1;
    /* 0644: anyone may see which images are in the drives - the GUI's
     * fallback reads it when the daemon is not answering. */
    (void) chmod(tmp, 0644);
    if (bad || rename(tmp, file) != 0) {
        int e = errno;

        (void) unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}
