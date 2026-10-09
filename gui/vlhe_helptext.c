/*
 * vlhe_helptext.c - the help file, read and split into sections.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * vlhe_helptext.h has the contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vlhe_helptext.h"
#include "vlhe_self.h"

/* "x *" -> "x", into `out'. The dirty mark the tabs carry. */
static void strip_mark(const char *in, char *out, int max)
{
    int n;

    out[0] = '\0';
    if (in == NULL)
        return;
    strncpy(out, in, max - 1);
    out[max - 1] = '\0';
    n = strlen(out);
    if (n >= 2 && out[n - 1] == '*' && out[n - 2] == ' ')
        out[n - 2] = '\0';
}

/* trim spaces both ends, in place */
static void trim(char *s)
{
    char *p = s;
    int n;

    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'
                     || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/*
 * IS THE LINE AT `p' (length `n') A HEADING? "== Title ==" exactly:
 * starts with "== ", ends with " ==". The file's own top title is
 * underlined with "=====", which has no space after the second '='
 * and so is not one. Fills `title' when it is.
 */
static int heading(const char *p, long n, char *title, int max)
{
    long len;

    if (n > 0 && p[n - 1] == '\r')
        n--;
    if (n < 7 || strncmp(p, "== ", 3) != 0
        || strncmp(p + n - 3, " ==", 3) != 0)
        return 0;
    len = n - 6;
    if (len >= max)
        len = max - 1;
    memcpy(title, p + 3, len);
    title[len] = '\0';
    trim(title);
    return title[0] != '\0';
}

void vlhe_help_parse(struct vlhe_help *h, char *text, long size)
{
    long pos = 0;

    memset(h, 0, sizeof *h);
    h->text = text;
    h->size = size;
    h->preamble = size;

    while (pos < size) {
        char *nl = memchr(text + pos, '\n', size - pos);
        long  n  = nl != NULL ? nl - (text + pos) : size - pos;
        char  title[VLHE_HELP_TITLE];

        if (heading(text + pos, n, title, sizeof title)) {
            struct vlhe_help_sect *s;
            char *slash;

            if (h->nsect == 0)
                h->preamble = pos;
            else
                h->sect[h->nsect - 1].len =
                    pos - h->sect[h->nsect - 1].off;

            /* PAST THE TABLE: fold into the last section rather than
             * drop it - the text still shows, under the last title. */
            if (h->nsect < VLHE_HELP_MAXSECT) {
                s = &h->sect[h->nsect++];
                strcpy(s->title, title);
                strcpy(s->page, title);
                s->tab[0] = '\0';
                slash = strstr(s->page, " / ");
                if (slash != NULL) {
                    strcpy(s->tab, slash + 3);
                    *slash = '\0';
                    trim(s->page);
                    trim(s->tab);
                }
                s->off = pos;
            }
        }
        pos += n + 1;
    }
    if (h->nsect > 0)
        h->sect[h->nsect - 1].len = size - h->sect[h->nsect - 1].off;
}

int vlhe_help_load(struct vlhe_help *h, const char *path)
{
    FILE *f;
    char *buf;
    long  cap = 32768, size = 0;
    size_t got;

    memset(h, 0, sizeof *h);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    buf = malloc(cap + 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    /* READ TO EOF rather than trusting a stat() size - simple, and the
     * file is tens of kilobytes. Capped at 1 MB: a help file bigger
     * than that is not ours. */
    while ((got = fread(buf + size, 1, cap - size, f)) > 0) {
        size += got;
        if (size == cap) {
            char *nb;
            if (cap >= 1024L * 1024L)
                break;
            nb = realloc(buf, cap * 2 + 1);
            if (nb == NULL)
                break;
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    buf[size] = '\0';
    vlhe_help_parse(h, buf, size);
    return 0;
}

void vlhe_help_free(struct vlhe_help *h)
{
    if (h->text != NULL)
        free(h->text);
    memset(h, 0, sizeof *h);
}

int vlhe_help_find(const struct vlhe_help *h, const char *page,
                   const char *tab)
{
    char p[VLHE_HELP_TITLE], t[VLHE_HELP_TITLE];
    int i;

    strip_mark(page, p, sizeof p);
    strip_mark(tab, t, sizeof t);
    if (p[0] == '\0')
        return -1;
    if (t[0] != '\0')
        for (i = 0; i < h->nsect; i++)
            if (strcmp(h->sect[i].page, p) == 0
                && strcmp(h->sect[i].tab, t) == 0)
                return i;
    for (i = 0; i < h->nsect; i++)
        if (strcmp(h->sect[i].page, p) == 0 && h->sect[i].tab[0] == '\0')
            return i;
    return -1;
}

/* append one tried path to `tried', and test it */
static int try_path(const char *path, char *out, int max,
                    char *tried, int tmax)
{
    /* ALREADY LOOKED - the build's datadir is /usr/local/share/vlhe
     * on a default build, and saying so twice reads like two places. */
    if (tried != NULL) {
        const char *q = tried;
        int n = strlen(path);
        while ((q = strstr(q, path)) != NULL) {
            if ((q == tried || q[-1] == '\n') && q[n] == '\n')
                return 0;
            q++;
        }
    }
    if (tried != NULL && (int)(strlen(tried) + strlen(path) + 2) < tmax) {
        strcat(tried, path);
        strcat(tried, "\n");
    }
    if (access(path, R_OK) != 0 || (int)strlen(path) >= max)
        return 0;
    strcpy(out, path);
    return 1;
}

int vlhe_help_locate(char *out, int max, char *tried, int tmax)
{
    static const char *const fixed[] = {
        "/usr/share/vlhe/" VLHE_HELP_FILE,
        "/usr/local/share/vlhe/" VLHE_HELP_FILE,
        NULL
    };
    char path[1024];
    const char *env;
    int i;

    if (tried != NULL && tmax > 0)
        tried[0] = '\0';

    env = getenv("VLHE_HELP");
    if (env != NULL && env[0] != '\0'
        && getuid() != 0 && getuid() == geteuid()
        && try_path(env, out, max, tried, tmax))
        return 1;

#ifdef VLHE_DATADIR
    if (strlen(VLHE_DATADIR) + strlen(VLHE_HELP_FILE) + 2 < sizeof path) {
        sprintf(path, "%s/%s", VLHE_DATADIR, VLHE_HELP_FILE);
        if (try_path(path, out, max, tried, tmax))
            return 1;
    }
#endif

    for (i = 0; fixed[i] != NULL; i++)
        if (try_path(fixed[i], out, max, tried, tmax))
            return 1;

    if (vlhe_self_path(VLHE_HELP_FILE, path, sizeof path)
        && try_path(path, out, max, tried, tmax))
        return 1;
    return 0;
}
