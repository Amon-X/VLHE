/*
 * sf2dump - print what sf2.c parsed, in a form that diffs cleanly
 * against another implementation's output.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * THE POINT IS THE COMPARISON, not the pretty-printing. Fields are
 * one per line, in file order, with no alignment that could hide a
 * difference in whitespace. See tests/compare.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sf2.h"
#include "sf2gen.h"

static void
warn_one(int gen, const char *name, int count, void *arg)
{
    (void) arg;
    printf("warn gen %d %s used %d\n", gen, name, count);
}

static void
usage(void)
{
    fprintf(stderr, "usage: sf2dump [-s] [-p] [-i] [-m] FILE.sf2\n");
    fprintf(stderr, "  default: the summary only\n");
    fprintf(stderr, "  -p  presets   -i  instruments   -s  samples\n");
    fprintf(stderr, "  -m  modulator and generator counts\n");
    fprintf(stderr, "  -w  warn about generators not acted on\n");
    fprintf(stderr, "  -z PRESET:KEY:VEL   resolve one note\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    sf2_file sf;
    FILE    *fp;
    int      i, a;
    int      want_p = 0, want_i = 0, want_s = 0, want_m = 0, want_w = 0;
    const char *zspec = NULL;
    const char *path = NULL;

    for (a = 1; a < argc; a++) {
        if      (strcmp(argv[a], "-p") == 0) want_p = 1;
        else if (strcmp(argv[a], "-i") == 0) want_i = 1;
        else if (strcmp(argv[a], "-s") == 0) want_s = 1;
        else if (strcmp(argv[a], "-m") == 0) want_m = 1;
        else if (strcmp(argv[a], "-w") == 0) want_w = 1;
        else if (strcmp(argv[a], "-z") == 0 && a + 1 < argc) zspec = argv[++a];
        else if (argv[a][0] == '-')          usage();
        else                                 path = argv[a];
    }
    if (path == NULL)
        usage();

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    if (sf2_load(&sf, fp) < 0) {
        fprintf(stderr, "sf2dump: %s: %s\n", path, sf2_error());
        fclose(fp);
        return 1;
    }

    printf("name %s\n", sf.name);
    printf("version %d.%02d\n", (int) sf.version_major,
                                (int) sf.version_minor);
    printf("presets %d\n", sf.npresets);
    printf("instruments %d\n", sf.ninsts);
    printf("samples %d\n", sf.nsamples);
    printf("samplepos %ld\n", sf.sample_pos);
    printf("samplesize %lu\n", sf.sample_size);

    if (want_m) {
        printf("pbags %d\n", sf.npbags);
        printf("ibags %d\n", sf.nibags);
        printf("pgens %d\n", sf.npgens);
        printf("igens %d\n", sf.nigens);
        printf("pmods %d\n", sf.npmods);
        printf("imods %d\n", sf.nimods);
    }

    if (want_p)
        for (i = 0; i < sf.npresets; i++)
            printf("preset %d bank %d prog %d bag %d %s\n",
                   i, (int) sf.presets[i].bank,
                   (int) sf.presets[i].preset,
                   (int) sf.presets[i].bag_index,
                   sf.presets[i].name);

    if (want_i)
        for (i = 0; i < sf.ninsts; i++)
            printf("inst %d bag %d %s\n",
                   i, (int) sf.insts[i].bag_index, sf.insts[i].name);

    if (want_s)
        for (i = 0; i < sf.nsamples; i++)
            printf("sample %d start %lu end %lu loop %lu %lu rate %lu"
                   " key %d corr %d link %d type %d %s\n",
                   i, sf.samples[i].start, sf.samples[i].end,
                   sf.samples[i].loop_start, sf.samples[i].loop_end,
                   sf.samples[i].sample_rate,
                   (int) sf.samples[i].original_key,
                   (int) sf.samples[i].correction,
                   (int) sf.samples[i].sample_link,
                   (int) sf.samples[i].sample_type,
                   sf.samples[i].name);

    if (want_w) {
        int n = sf2_zone_warn(&sf, warn_one, NULL);
        printf("unimplemented %d\n", n);
    }

    if (zspec != NULL) {
        int pre = 0, key = 60, vel = 100;
        sf2_zone z;

        if (sscanf(zspec, "%d:%d:%d", &pre, &key, &vel) != 3) {
            fprintf(stderr, "sf2dump: -z wants PRESET:KEY:VEL\n");
            sf2_free(&sf);
            fclose(fp);
            return 2;
        }

        i = sf2_zone_find(&sf, pre, key, vel, &z);
        if (i < 0) {
            printf("zone %d:%d:%d ERROR\n", pre, key, vel);
        } else if (i == 0) {
            printf("zone %d:%d:%d NONE\n", pre, key, vel);
        } else {
            int g;
            printf("zone %d:%d:%d sample %d %s\n", pre, key, vel,
                   z.sample_index,
                   z.sample_index >= 0 ? sf.samples[z.sample_index].name
                                       : "-");
            /* Only what the file actually set, so the output is a
             * statement about THIS soundfont rather than about our
             * default table. */
            for (g = 0; g < SF2_GEN_COUNT; g++)
                if (z.set[g])
                    printf("  gen %d %s %d\n", g, sf2_gen_name(g),
                           (int) z.gen[g]);
        }
    }

    sf2_free(&sf);
    fclose(fp);
    return 0;
}
