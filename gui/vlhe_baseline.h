/*
 * vlhe_baseline.h - how the machine was BEFORE VLHE first changed it.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * READ design/54 section 7h, Stage 3, FIRST. This is the interface.
 *
 * TWO RECORDS, TWO ROLES. The session files (vlhe_session.h) say what
 * VLHE MADE; the baseline says what to RESTORE - each path as it was
 * the first time VLHE ever touched it, on this machine. A path that
 * matches the baseline is the original state, not VLHE's work.
 *
 * WRITTEN WHEN A PATH IS FIRST RECORDED, NEVER REWRITTEN. The entry for
 * a path is added at the moment the first session record for it is
 * written - which is before the change, so the baseline always predates
 * VLHE's first change to that path, including paths a later config
 * adds. Only the user's explicit "update" on drift replaces an entry,
 * and the file it replaces is kept beside it with a timestamp.
 *
 * KEYED BY MACHINE: hostname plus the root filesystem's ext2 UUID (the
 * user's decision 3), in the file's NAME - so a portable copy carried
 * to a second machine starts a baseline of its own there instead of
 * seeing everything as drift.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_BASELINE_H
#define VLHE_BASELINE_H

#include <stddef.h>
#include "vlhe_backend.h"       /* VLHE_PATH_MAX */

/* How an entry's state is known. */
enum {
    VLHE_BASE_FOUND = 1,        /* read off the machine                  */
    VLHE_BASE_INFERRED,         /* worked out: MAKEDEV's fixed numbers,   */
                                /* or a session file's recorded prior     */
    VLHE_BASE_UNKNOWN,          /* VLHE's own leftover, original lost -   */
                                /* never guessed (/dev/cdrom)             */
    VLHE_BASE_DANGLING          /* FOUND, but a dsp link that may be      */
                                /* VLHE's leftover and cannot be shown to */
                                /* be: dangling, or already at vsound's   */
                                /* node with no session record. Reported  */
                                /* until the user restores the stock node */
                                /* or keeps it (design/54 7h Stage 3.5)   */
};

struct vlhe_base_ent {
    char what[VLHE_PATH_MAX];   /* the path                               */
    char state[VLHE_PATH_MAX];  /* "absent", "link T", "node M m" (char), */
                                /* "block M m", "other"                   */
    int  how;                   /* VLHE_BASE_*                            */
};

/* This machine's key, "<host>/<uuid>" - <uuid> is "nouuid" when the
 * root filesystem's superblock cannot be read. VLHE_MACHINE_KEY in the
 * environment overrides it (the host tests). */
const char *vlhe_baseline_key(void);

/* The baseline file for this machine: beside the session file,
 * `vlhe-baseline-<key>' with the key made filename-safe. */
const char *vlhe_baseline_path(void);

/* The UUID out of an ext2 superblock on `dev' (a device or an image),
 * as 8-4-4-4-12 hex. 0, or -1 when it is not ext2 or cannot be read. */
int vlhe_baseline_uuid_from(const char *dev, char *out, size_t max);

/* What is at `path' NOW, in the baseline's vocabulary. A node carries
 * its mode and owner after the numbers - "node 14 3 mode 0660 uid 0
 * gid 29" - since 2026-10-04 (design/55 R10); see vlhe_state_same(). */
void vlhe_baseline_describe(const char *path, char *state, size_t max);

/* " mode 0660 uid 0 gid 29" for a node's stat, the suffix
 * vlhe_baseline_describe() appends. */
void vlhe_state_perm_suffix(int mode, long uid, long gid, char *out);

/* ARE TWO STATES THE SAME THING? Compared WITHOUT the mode/owner
 * suffix, so a baseline or session record written before the suffix
 * existed still matches the node it describes, and drift means a
 * different node, link or absence - as it always did. */
int vlhe_state_same(const char *a, const char *b);

/* The recorded mode and owner, when the state carries them: 1 and
 * filled, 0 when it is an older record without them. */
int vlhe_state_perm(const char *state, int *mode, long *uid, long *gid);

/* The entry for `what': 1 found (copied to *e), 0 none, -1 unreadable. */
int vlhe_baseline_get(const char *what, struct vlhe_base_ent *e);

/* Add `what' if the baseline has no entry for it - written once.
 * 1 added, 0 already there (nothing changed), -1 could not write. */
int vlhe_baseline_add(const char *what, const char *state, int how);

/* Every entry, on the heap (free() it); NULL with *n 0 for none, *n -1
 * when the file exists and cannot be read. */
struct vlhe_base_ent *vlhe_baseline_read_all(int *n);

/* REPLACE the entry for `what' - the user's "update" on drift, and
 * nothing else. The file as it was is kept as `<file>.<stamp>' first.
 * 0, or -1 when it could not be kept or written. */
int vlhe_baseline_update(const char *what, const char *state, int how);

/* "FOUND" / "INFERRED" / "UNKNOWN" / "DANGLING". */
const char *vlhe_baseline_how_word(int how);

#endif
