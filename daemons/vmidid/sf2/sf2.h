/*
 * sf2.h - SoundFont 2.01 container reader.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * OUR OWN IMPLEMENTATION, written from the SF2.01 specification and
 * from the byte layout of real files. awesfx (GPL-2) and TiMidity
 * (GPL-2) are both in the project and both parse this format; neither
 * is copied here. They are used as ORACLES - see tests/ - because a
 * parser that agrees with two independent implementations on a real
 * 10 MB file is worth more than one that merely compiles.
 *
 * This project is heading for BSD-3, which is why that distinction
 * matters and is stated here rather than assumed.
 *
 * WHAT THIS IS AND IS NOT. It reads the CONTAINER: the RIFF chunk
 * tree and the nine pdta record arrays, into arrays that mirror the
 * file. It does NOT interpret zones, resolve generators, or apply the
 * layering rules - that is the next piece, and it is the larger half
 * (awesfx spends 639 lines here and 1234 on interpretation).
 *
 * SF2 ONLY, NOT SBK. TiMidity's reader also handles SoundFont 1.x,
 * where shdr records are 16 bytes rather than 46 and sample names
 * live in a separate chunk (its sffile.c:626-630). That format
 * predates 1996 and nothing this project targets produces it; a file
 * claiming version 1 is rejected rather than half-read.
 *
 * C89, no dependencies beyond stdio/stdlib/string, and it builds with
 * the target's gcc 2.95.2.
 */

#ifndef SF2_H
#define SF2_H

#include <stdio.h>

/*
 * THE ON-DISK RECORD SIZES ARE FIXED BY THE SPECIFICATION and are NOT
 * sizeof() of the structs below - a compiler is free to pad these and
 * gcc does. Every read goes field by field for that reason; see
 * sf2_read_* in sf2.c.
 *
 * Confirmed against Roland.SC-55.sf2: every pdta sub-chunk divides
 * exactly by the size named here.
 */
#define SF2_PHDR_SZ 38
#define SF2_BAG_SZ   4
#define SF2_MOD_SZ  10
#define SF2_GEN_SZ   4
#define SF2_INST_SZ 22
#define SF2_SHDR_SZ 46

/* A preset header: phdr. */
typedef struct {
    char           name[21];      /* 20 on disk, NUL added by us */
    unsigned short preset;
    unsigned short bank;
    unsigned short bag_index;     /* first pbag */
    unsigned long  library;
    unsigned long  genre;
    unsigned long  morphology;
} sf2_preset;

/* An instrument header: inst. */
typedef struct {
    char           name[21];
    unsigned short bag_index;     /* first ibag */
} sf2_inst;

/* A zone: pbag or ibag. */
typedef struct {
    unsigned short gen_index;
    unsigned short mod_index;
} sf2_bag;

/* A generator: pgen or igen. */
typedef struct {
    unsigned short oper;
    unsigned short amount;        /* union of ranges/short/ushort */
} sf2_gen;

/* A modulator: pmod or imod. Merged per SF2.01 9.5.1 by sf2mod.c
 * into each resolved zone's list (sf2gen.h, sf2_zone.mods) and
 * evaluated by the synth at note-on and on every controller event.
 * "Read and kept, not interpreted" until 2026-09-08 - design/18. */
typedef struct {
    unsigned short src;
    unsigned short dest;
    short          amount;
    unsigned short amt_src;
    unsigned short transform;
} sf2_mod;

/* A sample header: shdr. */
typedef struct {
    char           name[21];
    unsigned long  start;
    unsigned long  end;
    unsigned long  loop_start;
    unsigned long  loop_end;
    unsigned long  sample_rate;
    unsigned char  original_key;
    signed char    correction;
    unsigned short sample_link;
    unsigned short sample_type;
} sf2_sample;

typedef struct {
    /* INFO */
    char            name[257];        /* INAM */
    unsigned short  version_major;    /* ifil */
    unsigned short  version_minor;

    /* sdta - where the sample data is, not the data itself */
    long            sample_pos;
    unsigned long   sample_size;      /* bytes */

    /* pdta. Counts EXCLUDE the terminal record the spec requires at
     * the end of phdr, inst and shdr - see sf2.c. */
    sf2_preset     *presets;   int npresets;
    sf2_inst       *insts;     int ninsts;
    sf2_sample     *samples;   int nsamples;
    sf2_bag        *pbags;     int npbags;
    sf2_bag        *ibags;     int nibags;
    sf2_gen        *pgens;     int npgens;
    sf2_gen        *igens;     int nigens;
    sf2_mod        *pmods;     int npmods;
    sf2_mod        *imods;     int nimods;
} sf2_file;

/*
 * Returns 0 on success, negative on failure, and always leaves the
 * struct safe to pass to sf2_free.
 */
int  sf2_load(sf2_file *sf, FILE *fp);
void sf2_free(sf2_file *sf);

/* The last failure, for diagnostics. Never NULL. */
const char *sf2_error(void);

#endif /* SF2_H */
