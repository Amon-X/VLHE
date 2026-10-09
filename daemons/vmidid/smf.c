/*
 * smf.c - Standard MIDI File reader. See smf.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smf.h"

static char errbuf[256] = "no error";

const char *
smf_error(void)
{
    return errbuf;
}

static int
fail(const char *what)
{
    strncpy(errbuf, what, sizeof(errbuf) - 1);
    errbuf[sizeof(errbuf) - 1] = '\0';
    return -1;
}

/*
 * HOW MUCH MEMORY A LOAD MAY TAKE - 2026-10-04. Every event is kept, in
 * a working list while the tracks are read and a final list after, so a
 * load needs the file plus about 40 bytes an event. A Black MIDI file the
 * user found - 125 MB, 31 million events, 49,307 notes at once - needs
 * about 1.5 GB, three times a target machine's memory: the load would
 * fail at best and swap the machine for hours at worst. So the caller
 * may set a limit and the load refuses, with the numbers, as soon as it
 * would pass it - before the large allocation, not after. 0, the
 * default, is no limit (smfdump, the measuring tool).
 */
static unsigned long g_mem_limit;

void
smf_set_memory_limit(unsigned long bytes)
{
    g_mem_limit = bytes;
}


/* SMF IS BIG-ENDIAN, unlike RIFF. Read byte at a time for the same
 * reason sf2.c does: correct on either host, nothing to configure. */
static unsigned long
be32(const unsigned char *p)
{
    return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) |
           ((unsigned long) p[2] << 8)  |  (unsigned long) p[3];
}

static unsigned int
be16(const unsigned char *p)
{
    return ((unsigned int) p[0] << 8) | (unsigned int) p[1];
}

/*
 * A VARIABLE-LENGTH QUANTITY: seven bits per byte, high bit set on
 * all but the last. Bounded at four bytes, which is the spec's limit
 * - an unbounded loop here is how a malformed file becomes a hang.
 */
static int
read_vlq(const unsigned char *buf, long len, long *pos, unsigned long *out)
{
    unsigned long v = 0;
    int           n = 0;

    for (;;) {
        unsigned char b;
        if (*pos >= len)
            return -1;
        b = buf[(*pos)++];
        v = (v << 7) | (unsigned long) (b & 0x7f);
        if (!(b & 0x80))
            break;
        if (++n >= 4)
            return -1;
    }
    *out = v;
    return 0;
}

/*
 * ONE EVENT AS IT SITS IN A TRACK, before tempo is applied. Kept
 * separately because tracks are parsed one at a time and merged
 * after, and the merge has to be by TICK - a track's own timeline
 * means nothing next to another's.
 */
typedef struct {
    unsigned long tick;
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    unsigned long tempo;      /* non-zero for a tempo change */
    long          sx_off;     /* status 0xF0: body in smf->sysex */
    int           sx_len;
} raw_event;

/* A size for the refusal: MB from a megabyte up, KB below it - a
 * small limit (a test's) read "0 MB" otherwise. */
static void
size_words(char *out, double bytes)
{
    if (bytes >= 1048576.0)
        sprintf(out, "%lu MB", (unsigned long) (bytes / 1048576.0));
    else
        sprintf(out, "%lu KB", (unsigned long) (bytes / 1024.0));
}

/* Grow the working list to hold one more event, within the limit. */
static int
grow_raw(raw_event **raw, int *maxraw, long filelen)
{
    int newcap = *maxraw ? *maxraw * 2 : 1024;
    raw_event *g;

    if (g_mem_limit != 0) {
        /* the file, the old and new working lists during the realloc,
         * and the final list that follows at the same count */
        double need = (double) filelen
                      + (double) *maxraw * sizeof(raw_event)
                      + (double) newcap * (sizeof(raw_event) + sizeof(smf_event));

        if (need > (double) g_mem_limit) {
            char nb[32], lb[32];

            size_words(nb, need);
            size_words(lb, (double) g_mem_limit);
            sprintf(errbuf, "too big for this machine: more than %d events,"
                            " which needs over %s to load - this machine"
                            " allows %s", *maxraw, nb, lb);
            return -1;
        }
    }
    g = (raw_event *) realloc(*raw, sizeof(raw_event) * (size_t) newcap);
    if (g == NULL)
        return fail("out of memory");
    *raw = g;
    *maxraw = newcap;
    return 0;
}

static int
cmp_tick(const void *a, const void *b)
{
    const raw_event *x = (const raw_event *) a;
    const raw_event *y = (const raw_event *) b;

    if (x->tick < y->tick) return -1;
    if (x->tick > y->tick) return 1;
    /*
     * A TEMPO CHANGE AT THE SAME TICK MUST SORT FIRST, or the notes
     * sharing that tick are timed with the OLD tempo. qsort is not
     * stable in C89, so this cannot be left to insertion order.
     */
    if (x->tempo && !y->tempo) return -1;
    if (!x->tempo && y->tempo) return 1;
    return 0;
}

int
smf_load(smf_file *smf, FILE *fp)
{
    unsigned char *buf;
    long           len, pos;
    raw_event     *raw = NULL;
    int            nraw = 0, maxraw = 0;
    long           maxsx = 0;
    int            t, i;
    unsigned long  tempo = 500000;    /* the spec's default: 120 BPM */
    unsigned long  last_tick = 0;
    double         usec = 0.0;      /* smf.h: a double, whole microseconds */

    memset(smf, 0, sizeof(*smf));

    if (fseek(fp, 0L, SEEK_END) != 0)
        return fail("cannot seek the file");
    len = ftell(fp);
    if (len < 14)
        return fail("too short to be a MIDI file");
    rewind(fp);

    buf = (unsigned char *) malloc((size_t) len);
    if (buf == NULL)
        return fail("out of memory");
    if (fread(buf, 1, (size_t) len, fp) != (size_t) len) {
        free(buf);
        return fail("short read");
    }

    if (memcmp(buf, "MThd", 4) != 0) {
        free(buf);
        return fail("not a MIDI file (no MThd)");
    }

    smf->format   = (int) be16(buf + 8);
    smf->ntracks  = (int) be16(buf + 10);
    smf->division = (int) be16(buf + 12);

    if (smf->format == 2) {
        free(buf);
        return fail("format 2 is a set of independent songs, not parts");
    }
    /*
     * A NEGATIVE DIVISION IS SMPTE TIMING, a different clock entirely
     * (frames and subframes, not ticks per quarter). Rejected rather
     * than misread as a huge tick rate.
     */
    if (smf->division <= 0) {
        free(buf);
        return fail("SMPTE division is not supported");
    }

    /*
     * EVERY LENGTH IS CHECKED AGAINST WHAT IS LEFT BEFORE IT IS ADDED -
     * design/55 R13, 2026-10-04. be32() is unsigned and `long' is 32
     * bits on the target, so a length of 0x80000000 or more went
     * NEGATIVE when added: the header's sent the next read 8 bytes
     * before the buffer, and a track's made `end' fall below `pos', so
     * a valid track was silently skipped. Compared as unsigned against
     * the bytes remaining, a length that runs past the file is the
     * truncation it is.
     */
    {
        unsigned long hl = be32(buf + 4);

        if (hl > (unsigned long) (len - 8)) {
            free(buf);
            return fail("the header chunk runs past the end of the file");
        }
        pos = 8 + (long) hl;
    }

    for (t = 0; t < smf->ntracks; t++) {
        long          end;
        unsigned long tick = 0;
        int           running = -1;

        if (pos + 8 > len)
            break;                        /* fewer tracks than claimed */
        if (memcmp(buf + pos, "MTrk", 4) != 0) {
            free(buf); free(raw);
            return fail("expected an MTrk chunk");
        }
        {
            unsigned long tl = be32(buf + pos + 4);

            if (tl > (unsigned long) (len - pos - 8))
                end = len;                /* truncated: take what is there */
            else
                end = pos + 8 + (long) tl;
        }
        pos += 8;

        while (pos < end) {
            unsigned long delta, mlen;
            int           status, nbytes;

            if (read_vlq(buf, end, &pos, &delta) < 0)
                break;
            tick += delta;
            if (pos >= end)
                break;

            status = buf[pos];
            if (status & 0x80) {
                pos++;
                /*
                 * RUNNING STATUS IS CANCELLED BY A SYSTEM MESSAGE
                 * (0xF0 and above) but NOT by a channel message. A
                 * reader that keeps the old status across a SysEx
                 * mis-parses everything after it.
                 */
                running = (status < 0xf0) ? status : -1;
            } else if (running >= 0) {
                status = running;
            } else {
                break;                    /* data byte with no status */
            }

            if (status == 0xff) {         /* meta */
                int mtype;
                if (pos >= end) break;
                mtype = buf[pos++];
                if (read_vlq(buf, end, &pos, &mlen) < 0) break;
                if (mtype == 0x51 && mlen == 3 && pos + 3 <= end) {
                    if (nraw >= maxraw && grow_raw(&raw, &maxraw, len) < 0) {
                        free(buf); free(raw);
                        return -1;
                    }
                    raw[nraw].tick   = tick;
                    raw[nraw].status = 0;
                    raw[nraw].data1  = 0;
                    raw[nraw].data2  = 0;
                    raw[nraw].tempo  = ((unsigned long) buf[pos] << 16) |
                                       ((unsigned long) buf[pos+1] << 8) |
                                        (unsigned long) buf[pos+2];
                    raw[nraw].sx_off = 0;
                    raw[nraw].sx_len = 0;
                    nraw++;
                }
                pos += (long) mlen;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                /*
                 * SYSEX. The body follows a length; the final F7 is
                 * part of it in the file and is stripped here, since
                 * render_sysex takes the body alone. Kept in one pool
                 * with the event pointing into it. A 0xF7 escape (a
                 * continuation, or raw bytes) is not a message and is
                 * skipped.
                 */
                if (read_vlq(buf, end, &pos, &mlen) < 0) break;
                if (status == 0xf0 && mlen >= 2 && pos + (long) mlen <= end) {
                    long blen = (long) mlen;
                    if (buf[pos + mlen - 1] == 0xf7)
                        blen--;                     /* without the F7 */
                    if (smf->sysex_len + blen > maxsx) {
                        maxsx = (smf->sysex_len + blen) * 2 + 1024;
                        smf->sysex = (unsigned char *) realloc(smf->sysex, (size_t) maxsx);
                        if (smf->sysex == NULL) { free(buf); free(raw);
                            return fail("out of memory"); }
                    }
                    memcpy(smf->sysex + smf->sysex_len, buf + pos, (size_t) blen);
                    if (nraw >= maxraw && grow_raw(&raw, &maxraw, len) < 0) {
                        free(buf); free(raw);
                        return -1;
                    }
                    raw[nraw].tick   = tick;
                    raw[nraw].status = 0xf0;
                    raw[nraw].data1  = 0;
                    raw[nraw].data2  = 0;
                    raw[nraw].tempo  = 0;
                    raw[nraw].sx_off = smf->sysex_len;
                    raw[nraw].sx_len = (int) blen;
                    nraw++;
                    smf->sysex_len += blen;
                }
                pos += (long) mlen;
                continue;
            }

            nbytes = ((status & 0xf0) == 0xc0 ||
                      (status & 0xf0) == 0xd0) ? 1 : 2;
            if (pos + nbytes > end)
                break;

            if (nraw >= maxraw && grow_raw(&raw, &maxraw, len) < 0) {
                free(buf); free(raw);
                return -1;
            }
            raw[nraw].tick   = tick;
            raw[nraw].status = (unsigned char) status;
            raw[nraw].data1  = buf[pos];
            raw[nraw].data2  = (nbytes == 2) ? buf[pos + 1] : 0;
            raw[nraw].tempo  = 0;
            raw[nraw].sx_off = 0;
            raw[nraw].sx_len = 0;
            nraw++;
            pos += nbytes;
        }
        pos = end;
    }

    free(buf);

    if (nraw == 0) {
        free(raw);
        return fail("no events found");
    }

    qsort(raw, (size_t) nraw, sizeof(raw_event), cmp_tick);

    smf->events = (smf_event *) malloc(sizeof(smf_event) * (size_t) nraw);
    if (smf->events == NULL) {
        free(raw);
        return fail("out of memory");
    }

    /*
     * TICKS TO MICROSECONDS, ACCUMULATING ACROSS TEMPO CHANGES.
     *
     * Converting from tick 0 with the current tempo would be wrong the
     * moment a song changes tempo - every event after it would be
     * scaled as though the new tempo had applied from the start. So
     * the elapsed time carries forward and only the DELTA is scaled.
     */
    for (i = 0; i < nraw; i++) {
        /*
         * DIVIDE BEFORE MULTIPLYING, AND CARRY THE REMAINDER.
         *
         * `delta * tempo' overflows 32 bits: a 61440-tick gap at the
         * default tempo of 500000 needs 35, and a long has 32
         * unsigned. This test's note-off sits exactly there.
         *
         * WHAT IT DID ON THE TARGET: the product wrapped, the
         * note-off arrived at the wrong time - hundreds of
         * milliseconds instead of 32 seconds - and the voice was
         * released almost immediately. That is the silent-gap failure
         * in the SoundFont spec test: notes ending long before they
         * should. A 64-bit long absorbs the 35 bits, so every
         * workstation render was correct and every 32-bit one was
         * not; gcc 2.95.2 and gcc 3.2 produced byte-identical wrong
         * output, which is what proved the fault was ours.
         *
         * The fix keeps 32-bit arithmetic. delta/division scales
         * first, and the remainder is scaled separately and added, so
         * nothing is lost: for delta = q*division + r,
         *
         *     delta*tempo/division = q*tempo + r*tempo/division
         *
         * `r' is below division (at most 32767 by the SMF header), so
         * r*tempo is bounded by 32767 * tempo. That is why the tempo
         * clamp below matters - without it a malformed file could
         * still overflow this term.
         */
        {
            unsigned long delta = raw[i].tick - last_tick;
            unsigned long div   = (unsigned long) smf->division;
            unsigned long q     = delta / div;
            unsigned long r     = delta % div;

            /* q * tempo IN double: on a gap of more than 8589 quarter
             * notes at 120 BPM it passed 32 bits - only a bogus file,
             * but that is the file this must not wrap on. The
             * remainder term stays integer: it is bounded as above, and
             * keeping it integer keeps every time a whole number, the
             * same values as before (smfcheck.sh compares them). */
            usec += (double) q * (double) tempo
                    + (double) ((r * tempo) / div);
        }
        last_tick = raw[i].tick;

        if (raw[i].tempo) {
            tempo = raw[i].tempo;
            continue;                     /* not a playable event */
        }
        smf->events[smf->nevents].usec   = usec;
        smf->events[smf->nevents].status = raw[i].status;
        smf->events[smf->nevents].data1  = raw[i].data1;
        smf->events[smf->nevents].data2  = raw[i].data2;
        smf->events[smf->nevents].sx_off = raw[i].sx_off;
        smf->events[smf->nevents].sx_len = raw[i].sx_len;
        smf->nevents++;
    }
    smf->length_usec = usec;

    free(raw);
    return 0;
}

void
smf_free(smf_file *smf)
{
    free(smf->events);
    smf->events  = NULL;
    smf->nevents = 0;
    free(smf->sysex);
    smf->sysex     = NULL;
    smf->sysex_len = 0;
}
