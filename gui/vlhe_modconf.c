/*
 * vlhe_modconf.c - would opening a sound device node load a module?
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * vlhe_modconf.h has the reasoning. C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "vlhe_modconf.h"
#include "vlhe_status.h"

#define MAX_ALIAS   64

struct alias {
    char name[40];              /* char-major-14, sound-slot-0 ...  */
    char mod[40];               /* the module, or off / null        */
};

static struct alias g_al[MAX_ALIAS];
static int    g_nal;
static time_t g_when;           /* when g_al was read - 0 never      */
static char   g_from[256];      /* VLHE_MODCONF it was read from     */

/* Keep the aliases that matter here, from one `alias NAME MODULE' line. */
static void
take_line(const char *line)
{
    char name[64], mod[64];
    int  i;

    if (sscanf(line, " alias %63s %63s", name, mod) != 2)
        return;
    if (strcmp(name, "char-major-14") != 0
        && strncmp(name, "sound-slot-", 11) != 0
        && strncmp(name, "sound-service-", 14) != 0)
        return;
    for (i = 0; i < g_nal; i++)
        if (strcmp(g_al[i].name, name) == 0)
            break;              /* a later line overrides an earlier */
    if (i == g_nal) {
        if (g_nal == MAX_ALIAS)
            return;
        g_nal++;
    }
    strncpy(g_al[i].name, name, sizeof g_al[i].name - 1);
    g_al[i].name[sizeof g_al[i].name - 1] = '\0';
    strncpy(g_al[i].mod, mod, sizeof g_al[i].mod - 1);
    g_al[i].mod[sizeof g_al[i].mod - 1] = '\0';
}

static void
take_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f) != NULL)
        take_line(line);
    fclose(f);
}

/* `modprobe -c', the effective configuration - built-in aliases
 * included. 0 read, -1 could not run it. fork/exec rather than popen():
 * the target's headers hide popen() under the stricter feature macros
 * some of this tree builds with. */
static int
take_modprobe(void)
{
    int   fds[2], st, got = 0;
    pid_t kid;
    FILE *f;
    char  line[256];

    if (access("/sbin/modprobe", X_OK) != 0 || pipe(fds) != 0)
        return -1;
    kid = fork();
    if (kid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (kid == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0)
            _exit(127);
        close(fds[1]);
        execl("/sbin/modprobe", "modprobe", "-c", (char *) NULL);
        _exit(127);
    }
    close(fds[1]);
    f = fdopen(fds[0], "r");
    if (f == NULL) {
        close(fds[0]);
        (void) waitpid(kid, &st, 0);
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        take_line(line);
        got = 1;
    }
    fclose(f);
    (void) waitpid(kid, &st, 0);
    return (got && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void
load_config(void)
{
    const char *env = getenv("VLHE_MODCONF");
    time_t now = time(NULL);
    int i, have14 = 0;

    /* CACHED FOR A FEW SECONDS - a probe loop asks per minor - and read
     * afresh when the test file named changes. */
    if (g_when != 0 && now - g_when < 5 && now >= g_when
        && strcmp(g_from, env != NULL ? env : "") == 0)
        return;
    g_nal = 0;
    g_when = now;
    strncpy(g_from, env != NULL ? env : "", sizeof g_from - 1);
    g_from[sizeof g_from - 1] = '\0';
    if (env != NULL && *env != '\0')
        take_file(env);
    else if (take_modprobe() == 0)
        return;                 /* built-ins included already */
    else {
        take_file("/etc/conf.modules");
        take_file("/etc/modules.conf");
    }
    /* THE BUILT-IN char-major-14, which neither file shows - unless the
     * configuration set it itself (to `off', say). */
    for (i = 0; i < g_nal; i++)
        if (strcmp(g_al[i].name, "char-major-14") == 0)
            have14 = 1;
    if (!have14 && g_nal < MAX_ALIAS) {
        strcpy(g_al[g_nal].name, "char-major-14");
        strcpy(g_al[g_nal].mod, "sound");
        g_nal++;
    }
}

/* Would this alias load something? Not when it is off or null, or when
 * the module it names is loaded already. */
static int
alias_would_load(const char *name, char *why, size_t max)
{
    char mod[40];
    size_t l;
    int i;

    for (i = 0; i < g_nal; i++) {
        if (strcmp(g_al[i].name, name) != 0)
            continue;
        if (strcmp(g_al[i].mod, "off") == 0
            || strcmp(g_al[i].mod, "null") == 0)
            return 0;
        strcpy(mod, g_al[i].mod);
        l = strlen(mod);
        if (l > 2 && strcmp(mod + l - 2, ".o") == 0)
            mod[l - 2] = '\0';
        if (vlhe_status_module_loaded(mod))
            return 0;
        if (why != NULL && max > 0) {
            char w[100];

            sprintf(w, "alias %.40s %.40s", name, g_al[i].mod);
            strncpy(why, w, max - 1);
            why[max - 1] = '\0';
        }
        return 1;
    }
    return 0;
}

int
vlhe_modconf_open_would_load(int minor, char *why, size_t max)
{
    char name[40];
    int  unit = minor, chain;

    if (why != NULL && max > 0)
        why[0] = '\0';
    load_config();

    /* SOUNDCORE ABSENT: chrdev_open() asks for the major's module. */
    if (!vlhe_status_module_loaded("soundcore"))
        return alias_would_load("char-major-14", why, max);

    /* SOUNDCORE PRESENT: an empty minor asks for its slot and service,
     * with the dsp variants (chains 4 and 5) folded onto 3, as
     * soundcore_open() does. */
    chain = unit & 0x0F;
    if (chain == 4 || chain == 5) {
        unit = (unit & 0xF0) | 3;
        chain = 3;
    }
    sprintf(name, "sound-slot-%d", unit >> 4);
    if (alias_would_load(name, why, max))
        return 1;
    sprintf(name, "sound-service-%d-%d", unit >> 4, chain);
    return alias_would_load(name, why, max);
}
