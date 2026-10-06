/*
 * vlhe_fontscan.c - see vlhe_fontscan.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "vlhe_fontscan.h"

/* awesfx's DEFAULT_SF_PATH begins with the first of these
 * (awesfx-0.5.0/configure.in:27). The other two entries of its path,
 * `sfbank' and `/usr/local/lib/sfbank', are deliberately NOT scanned -
 * they predate the .sf2 extension and belong to AWE cards nobody here
 * has, so they would only ever be empty. */
const char *const vlhe_fontscan_defaults[VLHE_N_FONTDIR_DEFAULTS] = {
    "/usr/share/sounds/sf2",
    "/usr/local/share/sounds/sf2",
    "~/.vlhe/sf2"               /* expanded by the backend, not here */
};

int
vlhe_fontscan_is_sf2(const char *name)
{
    size_t n = strlen(name);
    const char *ext;

    if (n < 5)                  /* "x.sf2" is the shortest possible */
        return 0;
    ext = name + n - 4;

    return ext[0] == '.' &&
           (ext[1] == 's' || ext[1] == 'S') &&
           (ext[2] == 'f' || ext[2] == 'F') &&
           ext[3] == '2';
}

/* The part after the last slash. */
static const char *
basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

/* Case-insensitive name compare, for the basename dedupe. Written out
 * because strcasecmp is not C89. */
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

static int
already_have(char out[][VLHE_PATH_MAX], int n, const char *name)
{
    int i;

    for (i = 0; i < n; i++) {
        if (same_name(basename_of(out[i]), name))
            return 1;
    }
    return 0;
}

int
vlhe_fontscan(const char dirs[][VLHE_PATH_MAX], int ndirs,
              char out[][VLHE_PATH_MAX], int max)
{
    int n = 0;
    int d;

    for (d = 0; d < ndirs && n < max; d++) {
        DIR *dp;
        struct dirent *de;

        if (dirs[d][0] == '\0')
            continue;

        dp = opendir(dirs[d]);
        if (dp == NULL)
            continue;           /* absent is normal - see the header */

        while ((de = readdir(dp)) != NULL && n < max) {
            struct stat st;
            char full[VLHE_PATH_MAX];
            size_t need, dlen, nlen;

            if (!vlhe_fontscan_is_sf2(de->d_name))
                continue;

            /* +1 for the slash, +1 for the terminator. A name that
             * does not fit is SKIPPED rather than truncated - a
             * truncated path would name a file that does not exist
             * and fail later, somewhere less obvious.
             *
             * BUILT BY memcpy RATHER THAN sprintf, so the bound and
             * the write are one piece of code. With sprintf the check
             * is a separate statement a later edit can drift from -
             * and gcc says so, because it cannot see the link. */
            dlen = strlen(dirs[d]);

            /* NOT A SECOND SEPARATOR IF THERE IS ALREADY ONE - added
             * 2026-09-22, after a target journal showed
             * `/mnt/xfer//Roland.SC-55.sf2'. The directory came from
             * gtk_file_selection_get_filename(), which returns one
             * with a trailing slash; on_dir_ok() now strips it, and
             * this guards the paths ALREADY STORED that way and any
             * other writer of the setting.
             *
             * `/' IS THE CASE THAT NEEDS THE dlen > 1 TEST: stripping
             * its only slash would leave nothing to join to. */
            while (dlen > 1 && dirs[d][dlen - 1] == '/')
                dlen--;

            nlen = strlen(de->d_name);
            need = dlen + 1 + nlen + 1;
            if (need > sizeof full)
                continue;
            memcpy(full, dirs[d], dlen);
            /* "/" ALREADY ENDS IN ONE - appending would give "//". */
            if (dlen == 1 && dirs[d][0] == '/') {
                memcpy(full + 1, de->d_name, nlen);
                full[1 + nlen] = '\0';
            } else {
                full[dlen] = '/';
                memcpy(full + dlen + 1, de->d_name, nlen);
                full[dlen + 1 + nlen] = '\0';
            }

            /* A DIRECTORY CALLED something.sf2 IS NOT A FONT, and
             * stat also drops a dangling symlink - which would
             * otherwise sit in the list and fail at load. */
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            if (already_have(out, n, de->d_name))
                continue;

            strcpy(out[n], full);
            n++;
        }
        closedir(dp);
    }

    return n;
}
