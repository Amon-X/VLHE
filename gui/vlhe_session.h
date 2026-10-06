/*
 * vlhe_session.h - what THIS load changed, so the unload can put it
 * back without asking the configuration.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * READ design/40-session-state.md FIRST. It has the reasoning; this
 * is the interface.
 *
 * WHY IT EXISTS, IN ONE SENTENCE. The unload used to re-derive what
 * to undo from the CURRENT config - `vlhe_apply.c' read
 * `programs_use' at unload time and restored whatever path that named
 * - so changing a setting between the load and the unload aimed the
 * restore at a node the load had never touched, and the one it HAD
 * touched was never put back.
 *
 * IT BROKE A MACHINE ON 2026-09-25. Six apply/undo pairs, each
 * applying ten changes and undoing nine, every undo printing "the
 * machine is as it was before the apply" while leaving a symlink
 * behind. `/dev/dsp', `/dev/dsp1' and `/dev/dsp2' all ended pointing
 * at a node that no longer existed and the control centre could find
 * no sound card at all.
 * `tests/logs/2026-09-25-dsp-symlink-strand/' is the capture.
 *
 * SO EVERY RECORD CARRIES ITS PATH. That is the field `dsp.was' and
 * `cdrom.was' never had: they record WHAT a node was - `node 14 35',
 * `link /dev/hdb', `absent' - and never WHICH node, which is why the
 * restore had to ask the config in the first place.
 *
 * THIS IS NOT THE JOURNAL AND MUST NOT BECOME IT. The user's call,
 * 2026-09-25: "Dont touch the journal, this should be separate. The
 * journal was designed to be human readable." `vlhe_journal.h' is an
 * AUDIT record - write-only (there is no reader in its API), appended
 * across every run forever, and written as prose for a person. Undo
 * needs records it can parse, scoped to one session. A format serving
 * both serves neither.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_SESSION_H
#define VLHE_SESSION_H

#include "vlhe_backend.h"       /* VLHE_PATH_MAX */

/*
 * WHAT KIND OF CHANGE A RECORD DESCRIBES.
 *
 * The numbers are written into the file as words, not as integers -
 * see the format below - so these may be renumbered freely.
 */
enum {
    VLHE_SESS_NODE = 1,         /* a node we created outright       */
    VLHE_SESS_LINK,             /* a path we replaced with a symlink */
    VLHE_SESS_MODULE,           /* a module we loaded                */
    VLHE_SESS_DAEMON            /* a daemon we started               */
};

/*
 * HOW FAR A RECORD HAS GOT.
 *
 * OPEN is the state every record is born in and the only one that
 * means "the machine is still carrying this".
 */
enum {
    VLHE_SESS_OPEN = 0,         /* in effect                         */
    VLHE_SESS_UNDONE,           /* reversed; the machine is clean of it */
    VLHE_SESS_FAILED,           /* the undo tried and could not      */
    /* SOMEONE ELSE CHANGED IT while VLHE was loaded, so the undo left it
     * as found - design/54 7h, the user's decision 5 (2026-10-03): a
     * FINAL state, the user's change wins and is reported once, and the
     * session is filed with it, unlike FAILED, which keeps the file
     * live for the next unload to retry. */
    VLHE_SESS_CONFLICT
};

/* One line of the file, parsed. */
struct vlhe_sess_rec {
    int  kind;                  /* VLHE_SESS_*                       */
    int  state;                 /* OPEN / UNDONE / FAILED / CONFLICT */
    char what[VLHE_PATH_MAX];   /* the PATH, or the module/daemon name */
    char prior[VLHE_PATH_MAX];  /* what was there before - see below */
    char note[96];              /* free text: argv, or why it failed */
};

/*
 * `prior' IS THE HALF THAT MAKES A RESTORE POSSIBLE, and it is empty
 * for the kinds that need no restoring (a module is unloaded by name,
 * a daemon stopped by name). For VLHE_SESS_LINK it is exactly the
 * vocabulary `dsp.was' used, because that vocabulary was right - only
 * its filing was wrong:
 *
 *     node <major> <minor>     it was a real device node
 *     link <target>            it was a symlink somewhere else
 *     absent                   there was nothing there
 *     other                    something we do not know how to rebuild
 *
 * For VLHE_SESS_NODE it is the major and minor we created, so an
 * unload can tell OUR node from one that replaced it since.
 */

/* WHERE THE FILE IS. $VLHE_SESSION wins; else $VLHE_MODULE_DIR; else
 * beside the binary in trial mode; else /var/lib/vlhe. The same
 * ladder vlhe_dsp_saved_path() climbs, for the same reasons. */
const char *vlhe_session_path(void);

/* WHERE COMPLETED ONES GO - the same directory plus `sessions'. */
const char *vlhe_session_dir(void);

/*
 * RECORD A CHANGE, AFTER IT SUCCEEDED.
 *
 * NEVER BEFORE, for the journal's reason: a file listing what was
 * ATTEMPTED is a file that lies about the machine, and every caller
 * of this is a step that has just returned success.
 *
 * `prior' and `note' may be NULL. Appends, creating the file with its
 * header if it is not there. Returns 0, or -1 if nothing could be
 * written - which the caller should report rather than swallow.
 */
int vlhe_session_add(int kind, const char *what, const char *prior,
                     const char *note);

/*
 * READ THE WHOLE FILE. Fills `out' with at most `max' records in the
 * order they were written and returns how many, or -1 if the file
 * cannot be read. A MISSING FILE IS 0 AND NOT -1: nothing was
 * recorded, which is a different thing from being unable to look.
 *
 * THE UNLOAD WALKS THE RESULT BACKWARDS. Changes come off in the
 * reverse of the order they went on, which is what makes a daemon
 * stop before the module it holds open goes away.
 */
int vlhe_session_read(struct vlhe_sess_rec *out, int max);

/*
 * EVERY RECORD, HOWEVER MANY - design/54 D22, 2026-10-03. The callers
 * read into `rec[64]' and the 65th record onwards was never seen: on
 * 86Box a file of 85 records hid the one that held /dev/dsp's original
 * node, and the unload never restored it. Returns a malloc'd array the
 * caller free()s, with the count in *n_out; NULL with *n_out 0 when
 * nothing is recorded, NULL with *n_out -1 when the file cannot be
 * read (or memory runs out - never a short list). Indexes are the
 * ones vlhe_session_mark() takes.
 */
struct vlhe_sess_rec *vlhe_session_read_all(int *n_out);
/* The same for any session file - a filed one in sessions/, say. */
struct vlhe_sess_rec *vlhe_session_read_path(const char *path, int *n_out);

/* A `;' COMMENT LINE in the live file - kept by every rewrite and filed
 * with the session, so its history explains itself (the boot's
 * "found after an unclean shutdown", design/54 7h). 0, or -1. */
int vlhe_session_note(const char *text);

/*
 * MARK RECORD `i' - the index vlhe_session_read() returned it at.
 * Rewrites the file in place with the new state and note.
 *
 * NOT DELETED WHEN IT IS DONE. The user, 2026-09-25: "I dont think it
 * should be deleted at the end. Lines that are undone get marked one
 * way and ones that failed do not or marked another way." A file that
 * vanishes on success and survives on failure is a file whose absence
 * means two different things, and it would throw away the evidence in
 * exactly the case it exists for.
 */
int vlhe_session_mark(int i, int state, const char *note);

/*
 * CALLED WHEN AN UNDO FINISHES. If every record reads UNDONE the file
 * is complete: it is moved to `sessions/<YYYYMMDD-HHMMSS>' and the
 * next load starts a fresh one.
 *
 * A FILE WITH ANY `FAILED' LINE IS LEFT WHERE IT IS, because it still
 * describes state the machine is carrying and the next unload has to
 * find it and retry. That is what makes the live filename mean
 * something: if it exists, something is running or was left behind.
 *
 * Returns 1 if it rotated, 0 if it left the file alone, -1 on error.
 */
int vlhe_session_rotate(void);

/* (The portable-to-installed session pointer, vlhe_session_pointer(),
 * was removed 2026-10-04 - each build reads only its own session file.
 * vlhe_session.c says why.) */

#endif /* VLHE_SESSION_H */
