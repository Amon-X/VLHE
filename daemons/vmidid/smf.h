/*
 * smf.h - Standard MIDI File reader.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * A TEST HARNESS INPUT, NOT PART OF THE SYNTH. The synth's real input
 * is a byte stream from /dev/vmidi, produced by musserv from lxdoom's
 * MUS data. This exists so the renderer can be exercised on this
 * workstation, against files whose correct sound is already known,
 * without staging a bundle or carrying a CF card.
 *
 * It flattens all tracks into one time-ordered event list and
 * converts ticks to microseconds, honouring tempo changes - so a
 * caller replays events, not a file format.
 *
 * FORMATS 0 AND 1. Format 2 (independent sequences) is rejected
 * rather than mis-played: its tracks are separate songs, not parallel
 * parts, and flattening them would overlay unrelated music.
 *
 * C89, gcc 2.95.2 clean.
 */

#ifndef SMF_H
#define SMF_H

#include <stdio.h>

/*
 * SONG TIME IS A double, IN WHOLE MICROSECONDS - 2026-10-04. It was an
 * unsigned long, which is 32 bits on the target and wraps at 71:35, so
 * a longer file - or a bogus one claiming hours - wrapped to a short
 * one. A 64-bit integer was the first plan; `long long' is not C89 and
 * this directory builds -ansi -pedantic (the user's decision, Makefile),
 * so every use would be a 2.95.2 warning. A double is C89, needs no
 * libgcc division call, and holds whole microseconds exactly up to
 * 2^53 - about 285 years. smf_load() still adds whole microseconds, so
 * the values are the integers they always were.
 */
typedef struct {
    double         usec;      /* from the start of the song */
    unsigned char  status;    /* 0x80..0xEF, channel in the low nibble;
                                 0xF0 for a SysEx, whose body is
                                 sysex[sx_off .. sx_off + sx_len) */
    unsigned char  data1;
    unsigned char  data2;
    long           sx_off;
    int            sx_len;
} smf_event;

typedef struct {
    smf_event     *events;
    int            nevents;
    /*
     * SYSEX BODIES, F0 and the final F7 stripped, in file order -
     * design/22 step 2. An event with status 0xF0 points into this.
     * A 0xF7 "escape" event is not a message and is skipped.
     */
    unsigned char *sysex;
    long           sysex_len;
    double         length_usec;   /* see smf_event.usec */
    int            format;
    int            ntracks;
    int            division;  /* ticks per quarter note */
} smf_file;

/* 0 on success, negative on failure. Safe to smf_free either way. */
int  smf_load(smf_file *smf, FILE *fp);
/* The most memory a load may take, in bytes; 0 (the default) is no
 * limit. A load that would pass it fails, smf_error() giving the
 * numbers - before the large allocation. smf2wav sets half the
 * machine's memory. */
void smf_set_memory_limit(unsigned long bytes);
void smf_free(smf_file *smf);

const char *smf_error(void);

#endif /* SMF_H */
