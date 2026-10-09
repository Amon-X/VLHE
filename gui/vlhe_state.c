/*
 * vlhe_state.c - see vlhe_state.h.
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 * Part of VLHE. See LICENSE for the full license text.
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <string.h>

#include "vlhe_backend.h"
#include "vlhe_state.h"
#include "vlhe_strings.h"

/*
 * THE DESIGN, the user's, 2026-10-07. Each page says in its button row
 * whether its part of VLHE is up: HEALTHY, BOTH HALVES ("vmidi loaded,
 * vmidid running"); a PROBLEM, ONLY THE PROBLEM, with a warning mark
 * at each end - never colour, for accessibility - and "see Status", where
 * the full account is. The explanations used to sit on each page and
 * kept pushing pages into a scrollbar.
 *
 * NOT LOADED IS A STATE, NOT A FAULT. A machine that has not loaded
 * VLHE, or a part it does not use, is not broken; "vmidi not loaded"
 * is said plainly. A module that IS loaded with its daemon down is the
 * fault - the drives or the channel exist and nothing serves them -
 * and that is what gets the mark.
 */

static const char *const g_module[] = { NULL, "vsound", "vmidi", "vdisc" };
static const char *const g_daemon[] = { NULL, "vsoundd", "vmidid", "vdiscd" };

/* "vdiscd" matches "vdiscd 0", "vdiscd 2"; "vsoundd" only itself. */
static int is_daemon_of(const char *name, const char *d)
{
    size_t n = strlen(d);

    return strncmp(name, d, n) == 0 && (name[n] == '\0' || name[n] == ' ');
}

int
vlhe_state_line(int part, char *out, size_t max)
{
    struct vlhe_component c[VLHE_MAX_COMPONENTS];
    char line[96];
    int n, i, loaded = 0, ndaemon = 0;
    const char *down = NULL;

    out[0] = '\0';
    if (part <= VLHE_STATE_NONE || part > VLHE_STATE_CD)
        return VLHE_STATE_EMPTY;

    n = vlhe_components(c, VLHE_MAX_COMPONENTS);
    for (i = 0; i < n; i++) {
        if (strcmp(c[i].name, g_module[part]) == 0)
            loaded = c[i].present;
        else if (is_daemon_of(c[i].name, g_daemon[part])) {
            ndaemon++;
            if (!c[i].present && down == NULL)
                down = c[i].name;
        }
    }

    if (!loaded) {
        sprintf(line, FMT_STATE_NOT_LOADED, g_module[part]);
        strncpy(out, line, max - 1);
        out[max - 1] = '\0';
        return VLHE_STATE_DOWN;
    }
    if (down != NULL) {
        sprintf(line, FMT_STATE_NOT_RUNNING, down);
        strncpy(out, line, max - 1);
        out[max - 1] = '\0';
        return VLHE_STATE_PROBLEM;
    }
    /* CD WITH NO DRIVE ATTACHED lists no vdiscd at all: nothing to run,
     * and nothing wrong - the module alone is the honest line. */
    if (ndaemon == 0)
        sprintf(line, FMT_STATE_LOADED, g_module[part]);
    else
        sprintf(line, FMT_STATE_UP, g_module[part], g_daemon[part]);
    strncpy(out, line, max - 1);
    out[max - 1] = '\0';
    return VLHE_STATE_UP;
}

int
vlhe_state_any_problem(void)
{
    char tmp[96];
    int p;

    for (p = VLHE_STATE_SOUND; p <= VLHE_STATE_CD; p++)
        if (vlhe_state_line(p, tmp, sizeof tmp) == VLHE_STATE_PROBLEM)
            return 1;
    return 0;
}
