/*
 * vtrace_core.c - see vtrace_core.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * `strlen' and `memcpy' are the only things it needs from outside:
 * the kernel's <linux/string.h> in a module, <string.h> in the host
 * test - the one #ifdef in the file. C89.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "vtrace_core.h"

void
vtrace_ring_init(struct vtrace_ring *r, char *mem, unsigned long nslots)
{
    r->slot   = mem;
    r->nslots = nslots;
    r->seq    = 0;
}

static char *
vtrace_slot_of(const struct vtrace_ring *r, unsigned long seq)
{
    return r->slot + (seq % r->nslots) * VTRACE_SLOT;
}

void
vtrace_ring_put(struct vtrace_ring *r, const char *text, unsigned long len)
{
    char *s;

    if (!r->slot || r->nslots == 0)
        return;
    s = vtrace_slot_of(r, r->seq);
    /* CUT, NEVER SPLIT: the slot keeps room for the newline and NUL. */
    if (len > VTRACE_SLOT - 2)
        len = VTRACE_SLOT - 2;
    memcpy(s, text, len);
    s[len]     = '\n';
    s[len + 1] = '\0';
    r->seq++;
}

unsigned long
vtrace_ring_oldest(const struct vtrace_ring *r)
{
    return r->seq > r->nslots ? r->seq - r->nslots : 0;
}

void
vtrace_pos_init(struct vtrace_pos *p, const struct vtrace_ring *r)
{
    p->next = vtrace_ring_oldest(r);
}

int
vtrace_ring_avail(const struct vtrace_ring *r, const struct vtrace_pos *p)
{
    return p->next < r->seq;
}

/* An unsigned number in decimal; returns its length. No sprintf here,
 * so the core needs nothing from a library. */
static unsigned long
vtrace_utoa(char *out, unsigned long v)
{
    char tmp[24];
    unsigned long n = 0, i;

    do {
        tmp[n++] = (char) ('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

unsigned long
vtrace_ring_read(const struct vtrace_ring *r, struct vtrace_pos *p,
                 char *out, unsigned long max)
{
    unsigned long done = 0, oldest, len;
    const char *s;

    if (!r->slot || r->nslots == 0)
        return 0;

    /* OVERWRITTEN WHILE AWAY: say how many, then resume at the oldest
     * line still held. The line is built by hand - "[" number " lines
     * lost]\n" - 32 bytes at most. */
    oldest = vtrace_ring_oldest(r);
    if (p->next < oldest) {
        char line[40];
        unsigned long n = 0;

        line[n++] = '[';
        n += vtrace_utoa(line + n, oldest - p->next);
        memcpy(line + n, " lines lost]\n", 13);
        n += 13;
        if (n > max)
            return 0;
        memcpy(out, line, n);
        done += n;
        p->next = oldest;
    }

    while (p->next < r->seq) {
        s = vtrace_slot_of(r, p->next);
        len = strlen(s);
        if (done + len > max)
            break;                  /* whole lines only */
        memcpy(out + done, s, len);
        done += len;
        p->next++;
    }
    return done;
}
