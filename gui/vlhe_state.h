/*
 * vlhe_state.h - one line saying whether a page's part of VLHE is up.
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Shown in the middle of the button row (vlhe_cc.c) and used for the
 * Status entry's marker, so the two cannot disagree. Built from
 * vlhe_components(), the same list the Status page shows.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_STATE_H
#define VLHE_STATE_H

#include <stddef.h>

/* Which part a page belongs to. */
enum {
    VLHE_STATE_NONE = 0,    /* Status, Render: nothing to report     */
    VLHE_STATE_SOUND,       /* vsound + vsoundd                      */
    VLHE_STATE_MIDI,        /* vmidi + vmidid                        */
    VLHE_STATE_CD           /* vdisc + every vdiscd N                */
};

/* What vlhe_state_line() found. */
enum {
    VLHE_STATE_EMPTY = 0,   /* nothing to say (VLHE_STATE_NONE)      */
    VLHE_STATE_UP,          /* "vmidi loaded, vmidid running"        */
    VLHE_STATE_DOWN,        /* "vmidi not loaded" - a state, not a fault */
    VLHE_STATE_PROBLEM      /* "vmidid not running" (+ "See Status.") */
};

/* The line for `part', into out (max bytes). Returns a VLHE_STATE_*
 * result code; out is "" for VLHE_STATE_EMPTY. */
int vlhe_state_line(int part, char *out, size_t max);

/* Non-zero when any part has a PROBLEM - for the Status marker. */
int vlhe_state_any_problem(void);

#endif
