/*
 * smfdump - what smf.c parsed, in a form that diffs cleanly.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Same role as sf2dump: the comparison is the point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smf.h"

int
main(int argc, char **argv)
{
    smf_file smf;
    FILE    *fp;
    int      i, summary = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: smfdump [-s] FILE.mid\n");
        fprintf(stderr, "  -s  summary only (polyphony, channels, programs)\n");
        return 2;
    }
    if (strcmp(argv[1], "-s") == 0) { summary = 1; argv++; argc--; }

    fp = fopen(argv[1], "rb");
    if (fp == NULL) { perror(argv[1]); return 1; }

    if (smf_load(&smf, fp) < 0) {
        fprintf(stderr, "smfdump: %s: %s\n", argv[1], smf_error());
        fclose(fp);
        return 1;
    }
    fclose(fp);

    if (summary) {
        int  on = 0, peak = 0, notes = 0;
        int  chan[16], prog[128], cc[128];
        int  n;

        for (n = 0; n < 16; n++)  chan[n] = 0;
        for (n = 0; n < 128; n++) prog[n] = cc[n] = 0;

        for (i = 0; i < smf.nevents; i++) {
            int k = smf.events[i].status & 0xf0;
            chan[smf.events[i].status & 0x0f] = 1;
            if (k == 0x90 && smf.events[i].data2 > 0) {
                on++; notes++;
                if (on > peak) peak = on;
            } else if (k == 0x80 ||
                       (k == 0x90 && smf.events[i].data2 == 0)) {
                if (on > 0) on--;
            } else if (k == 0xc0) {
                prog[smf.events[i].data1 & 0x7f] = 1;
            } else if (k == 0xb0) {
                cc[smf.events[i].data1 & 0x7f]++;
            }
        }

        printf("format %d tracks %d division %d\n",
               smf.format, smf.ntracks, smf.division);
        printf("events %d\n", smf.nevents);
        printf("notes %d\n", notes);
        printf("polyphony %d\n", peak);
        printf("length_ms %lu\n", (unsigned long) (smf.length_usec / 1000.0));
        printf("channels");
        for (n = 0; n < 16; n++) if (chan[n]) printf(" %d", n);
        printf("\n");
        printf("programs");
        for (n = 0; n < 128; n++) if (prog[n]) printf(" %d", n);
        printf("\n");
        printf("controllers");
        for (n = 0; n < 128; n++) if (cc[n]) printf(" %d:%d", n, cc[n]);
        printf("\n");
    } else {
        for (i = 0; i < smf.nevents; i++)
            /* THE SAME TEXT AS BEFORE for any time an unsigned long
             * holds; past it, the whole number as a double prints it. */
            if (smf.events[i].usec < 4294967295.0)
                printf("%lu %02x %d %d\n",
                       (unsigned long) smf.events[i].usec,
                       (unsigned) smf.events[i].status,
                       (int) smf.events[i].data1,
                       (int) smf.events[i].data2);
            else
                printf("%.0f %02x %d %d\n", smf.events[i].usec,
                   (unsigned) smf.events[i].status,
                   (int) smf.events[i].data1,
                   (int) smf.events[i].data2);
    }

    smf_free(&smf);
    return 0;
}
