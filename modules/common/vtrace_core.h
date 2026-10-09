/*
 * vtrace_core.h - the trace ring: fixed text slots, a sequence number,
 * and readers that each keep their own place.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/54 section 8 as revised by 8f, 2026-10-08. PURE ARITHMETIC
 * over memory the caller supplies: no kernel headers, no locking, no
 * I/O - the kernel glue (vtrace.c) holds the lock around every call
 * and the host test (host/tests/test_vtrace.c) drives it with a plain
 * array. That split is the same one autovoice.c has with vmidid.
 *
 * ONE LINE PER SLOT, TEXT, NOT RECORDS. A slot is VTRACE_SLOT bytes:
 * the line, a newline, a NUL. A line longer than fits is cut, never
 * split across slots. The writer overwrites the oldest slot when the
 * ring is full; the sequence number counts every line ever written, so
 * a reader whose place has been overwritten knows exactly how many it
 * missed and is told so IN THE STREAM - "[N lines lost]" - rather than
 * silently skipped (CLAUDE.md section 2: a loss is said).
 *
 * C89.
 */
#ifndef VTRACE_CORE_H
#define VTRACE_CORE_H

#define VTRACE_SLOT   96      /* bytes per slot, newline and NUL included */

struct vtrace_ring {
    char          *slot;      /* nslots x VTRACE_SLOT, the caller's   */
    unsigned long  nslots;
    unsigned long  seq;       /* lines written so far; the next one's number */
};

/* A reader's place: the number of the next line it wants. */
struct vtrace_pos {
    unsigned long next;
};

void vtrace_ring_init(struct vtrace_ring *r, char *mem, unsigned long nslots);

/* Store `len' bytes of `text' (no newline) as line number r->seq, cut
 * to fit the slot, and advance. */
void vtrace_ring_put(struct vtrace_ring *r, const char *text,
                     unsigned long len);

/* The number of the oldest line still held (seq when empty). */
unsigned long vtrace_ring_oldest(const struct vtrace_ring *r);

/* Start a reader at the oldest line still held. */
void vtrace_pos_init(struct vtrace_pos *p, const struct vtrace_ring *r);

/* Has the reader anything to read? */
int vtrace_ring_avail(const struct vtrace_ring *r, const struct vtrace_pos *p);

/*
 * Copy whole lines the reader has not seen into `out', at most `max'
 * bytes, and advance the reader. A line that would not fit is left
 * for the next call, so `out' always ends at a line end. If the
 * reader's place has been overwritten, the first line copied is
 * "[N lines lost]\n" and the reader moves to the oldest line held.
 * Returns the bytes copied - 0 when nothing new (or `max' is too small
 * for the next line).
 */
unsigned long vtrace_ring_read(const struct vtrace_ring *r,
                               struct vtrace_pos *p, char *out,
                               unsigned long max);

#endif /* VTRACE_CORE_H */
