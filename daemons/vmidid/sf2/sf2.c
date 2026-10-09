/*
 * sf2.c - SoundFont 2.01 container reader. See sf2.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Written from the SF2.01 specification and from the byte layout of
 * real files; awesfx and TiMidity are used as oracles in tests/, not
 * copied. C89, gcc 2.95.2 clean.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sf2.h"

static char sf2_errbuf[256] = "no error";

const char *
sf2_error(void)
{
    return sf2_errbuf;
}

static int
fail(const char *what)
{
    /* strncpy, not sprintf: `what' is ours in every call site, but a
     * fixed buffer with an unbounded format is how these become bugs. */
    strncpy(sf2_errbuf, what, sizeof(sf2_errbuf) - 1);
    sf2_errbuf[sizeof(sf2_errbuf) - 1] = '\0';
    return -1;
}

/*
 * LITTLE-ENDIAN READS, BYTE BY BYTE.
 *
 * Not fread() into a short. Two separate reasons, and the second is
 * the one that bites on a machine nobody tests:
 *
 *   1. RIFF is little-endian by definition, so a big-endian host must
 *      swap. Doing it a byte at a time is correct everywhere and needs
 *      no WORDS_BIGENDIAN to be configured right.
 *   2. The on-disk records are PACKED and the structs are not. shdr is
 *      46 bytes on disk; sizeof(sf2_sample) is larger on i386 because
 *      the compiler aligns the longs. Reading a struct whole would
 *      work on neither layout reliably.
 *
 * Every one returns 0 on EOF, which the callers detect through the
 * chunk sizes rather than through the value - a legitimate 0 is
 * indistinguishable otherwise.
 */
static unsigned short
rd16(FILE *fp)
{
    int a = getc(fp), b = getc(fp);
    if (a == EOF || b == EOF)
        return 0;
    return (unsigned short) (a | (b << 8));
}

static unsigned long
rd32(FILE *fp)
{
    unsigned long a = (unsigned long) rd16(fp);
    unsigned long b = (unsigned long) rd16(fp);
    return a | (b << 16);
}

/* A 4-character chunk id, not NUL-terminated on disk. */
static int
rdid(FILE *fp, char *out)
{
    if (fread(out, 1, 4, fp) != 4)
        return -1;
    out[4] = '\0';
    return 0;
}

/*
 * A FIXED-WIDTH NAME FIELD.
 *
 * The spec says 20 bytes, and says it is NUL-terminated - but real
 * files exist where all 20 are used with no terminator, so the field
 * is read whole and terminated by us. Reading to the first NUL would
 * leave the file position wrong on exactly those files, which is the
 * kind of bug that only shows up on someone else's soundfont.
 */
static void
rdname(FILE *fp, char *out, int width)
{
    if (fread(out, 1, (size_t) width, fp) != (size_t) width)
        out[0] = '\0';
    out[width] = '\0';
}

static int
skip(FILE *fp, long n)
{
    return fseek(fp, n, SEEK_CUR);
}

/*
 * THE TERMINAL RECORD.
 *
 * phdr, inst and shdr each end with a dummy record the spec requires:
 * "EOP", "EOI" and "EOS". It is not a preset - its bag_index is the
 * one-past-the-end marker that gives the LAST real record its extent,
 * so it must be read and then not counted.
 *
 * Getting this wrong by one is the classic SF2 bug in both
 * directions: count it and every consumer sees a phantom instrument;
 * drop it before use and the last real one has no end.
 *
 * We keep the array at the full on-disk length - the zone walker
 * needs record[i+1].bag_index - and report the count WITHOUT the
 * terminator. So sf->npresets is what a user means by "presets", and
 * presets[sf->npresets] is still valid to read a bag_index from.
 */

static int
read_phdr(sf2_file *sf, unsigned long size, FILE *fp)
{
    int n, i;

    if (size % SF2_PHDR_SZ)
        return fail("phdr size is not a multiple of 38");
    n = (int) (size / SF2_PHDR_SZ);
    if (n < 1)
        return fail("phdr has no terminal record");

    sf->presets = (sf2_preset *) malloc(sizeof(sf2_preset) * (size_t) n);
    if (sf->presets == NULL)
        return fail("out of memory reading phdr");

    for (i = 0; i < n; i++) {
        rdname(fp, sf->presets[i].name, 20);
        sf->presets[i].preset     = rd16(fp);
        sf->presets[i].bank       = rd16(fp);
        sf->presets[i].bag_index  = rd16(fp);
        sf->presets[i].library    = rd32(fp);
        sf->presets[i].genre      = rd32(fp);
        sf->presets[i].morphology = rd32(fp);
    }
    sf->npresets = n - 1;               /* less the terminal EOP */
    return 0;
}

static int
read_inst(sf2_file *sf, unsigned long size, FILE *fp)
{
    int n, i;

    if (size % SF2_INST_SZ)
        return fail("inst size is not a multiple of 22");
    n = (int) (size / SF2_INST_SZ);
    if (n < 1)
        return fail("inst has no terminal record");

    sf->insts = (sf2_inst *) malloc(sizeof(sf2_inst) * (size_t) n);
    if (sf->insts == NULL)
        return fail("out of memory reading inst");

    for (i = 0; i < n; i++) {
        rdname(fp, sf->insts[i].name, 20);
        sf->insts[i].bag_index = rd16(fp);
    }
    sf->ninsts = n - 1;                 /* less the terminal EOI */
    return 0;
}

static int
read_shdr(sf2_file *sf, unsigned long size, FILE *fp)
{
    int n, i;

    if (size % SF2_SHDR_SZ)
        return fail("shdr size is not a multiple of 46");
    n = (int) (size / SF2_SHDR_SZ);
    if (n < 1)
        return fail("shdr has no terminal record");

    sf->samples = (sf2_sample *) malloc(sizeof(sf2_sample) * (size_t) n);
    if (sf->samples == NULL)
        return fail("out of memory reading shdr");

    for (i = 0; i < n; i++) {
        int c;
        rdname(fp, sf->samples[i].name, 20);
        sf->samples[i].start        = rd32(fp);
        sf->samples[i].end          = rd32(fp);
        sf->samples[i].loop_start   = rd32(fp);
        sf->samples[i].loop_end     = rd32(fp);
        sf->samples[i].sample_rate  = rd32(fp);
        c = getc(fp);
        sf->samples[i].original_key = (unsigned char) (c == EOF ? 0 : c);
        /* PITCH CORRECTION IS SIGNED, in cents. Read as unsigned and
         * a flat sample becomes a very sharp one. */
        c = getc(fp);
        sf->samples[i].correction   = (signed char) (c == EOF ? 0 : c);
        sf->samples[i].sample_link  = rd16(fp);
        sf->samples[i].sample_type  = rd16(fp);
    }
    sf->nsamples = n - 1;               /* less the terminal EOS */
    return 0;
}

static int
read_bags(sf2_bag **out, int *count, unsigned long size, FILE *fp,
          const char *what)
{
    int n, i;

    if (size % SF2_BAG_SZ)
        return fail(what);
    n = (int) (size / SF2_BAG_SZ);

    *out = (sf2_bag *) malloc(sizeof(sf2_bag) * (size_t) (n > 0 ? n : 1));
    if (*out == NULL)
        return fail("out of memory reading a bag chunk");

    for (i = 0; i < n; i++) {
        (*out)[i].gen_index = rd16(fp);
        (*out)[i].mod_index = rd16(fp);
    }
    /* NOT decremented. A bag array's last entry is a real terminator
     * too, but every consumer indexes it directly from a header's
     * bag_index rather than iterating, so the honest count is the
     * on-disk one. */
    *count = n;
    return 0;
}

static int
read_gens(sf2_gen **out, int *count, unsigned long size, FILE *fp,
          const char *what)
{
    int n, i;

    if (size % SF2_GEN_SZ)
        return fail(what);
    n = (int) (size / SF2_GEN_SZ);

    *out = (sf2_gen *) malloc(sizeof(sf2_gen) * (size_t) (n > 0 ? n : 1));
    if (*out == NULL)
        return fail("out of memory reading a generator chunk");

    for (i = 0; i < n; i++) {
        (*out)[i].oper   = rd16(fp);
        (*out)[i].amount = rd16(fp);
    }
    *count = n;
    return 0;
}

static int
read_mods(sf2_mod **out, int *count, unsigned long size, FILE *fp,
          const char *what)
{
    int n, i;

    if (size % SF2_MOD_SZ)
        return fail(what);
    n = (int) (size / SF2_MOD_SZ);

    *out = (sf2_mod *) malloc(sizeof(sf2_mod) * (size_t) (n > 0 ? n : 1));
    if (*out == NULL)
        return fail("out of memory reading a modulator chunk");

    for (i = 0; i < n; i++) {
        (*out)[i].src       = rd16(fp);
        (*out)[i].dest      = rd16(fp);
        (*out)[i].amount    = (short) rd16(fp);
        (*out)[i].amt_src   = rd16(fp);
        (*out)[i].transform = rd16(fp);
    }
    *count = n;
    return 0;
}

static int
read_info(sf2_file *sf, unsigned long size, FILE *fp)
{
    long end = ftell(fp) + (long) size - 4;   /* the LIST id is read */

    while (ftell(fp) < end) {
        char          id[5];
        unsigned long sz;

        if (rdid(fp, id) < 0)
            return fail("truncated INFO chunk");
        sz = rd32(fp);

        if (strcmp(id, "INAM") == 0) {
            unsigned long take = sz;
            if (take > sizeof(sf->name) - 1)
                take = sizeof(sf->name) - 1;
            if (fread(sf->name, 1, (size_t) take, fp) != take)
                return fail("truncated INAM");
            sf->name[take] = '\0';
            if (skip(fp, (long) (sz - take)) < 0)
                return fail("seek failed in INFO");
        } else if (strcmp(id, "ifil") == 0 && sz >= 4) {
            sf->version_major = rd16(fp);
            sf->version_minor = rd16(fp);
            if (skip(fp, (long) sz - 4) < 0)
                return fail("seek failed in ifil");
        } else {
            if (skip(fp, (long) sz) < 0)
                return fail("seek failed in INFO");
        }

        /* RIFF PADS ODD-SIZED CHUNKS TO EVEN. The pad byte is not
         * counted in the size, so skipping only `sz' leaves every
         * later read one byte out - and INFO is full of odd-length
         * strings, so this is not a corner case. */
        if (sz & 1) {
            if (skip(fp, 1L) < 0)
                return fail("seek failed on a RIFF pad byte");
        }
    }
    return 0;
}

static int
read_sdta(sf2_file *sf, unsigned long size, FILE *fp)
{
    long end = ftell(fp) + (long) size - 4;

    while (ftell(fp) < end) {
        char          id[5];
        unsigned long sz;

        if (rdid(fp, id) < 0)
            return fail("truncated sdta chunk");
        sz = rd32(fp);

        if (strcmp(id, "smpl") == 0) {
            /*
             * THE POSITION, NOT THE DATA. FluidR3's smpl chunk is
             * 148 MB; a synth streams or mmaps it and no caller wants
             * it in a malloc.
             */
            sf->sample_pos  = ftell(fp);
            sf->sample_size = sz;
        }
        if (skip(fp, (long) sz) < 0)
            return fail("seek failed in sdta");
        if (sz & 1) {
            if (skip(fp, 1L) < 0)
                return fail("seek failed on a RIFF pad byte");
        }
    }
    return 0;
}

static int
read_pdta(sf2_file *sf, unsigned long size, FILE *fp)
{
    long end = ftell(fp) + (long) size - 4;

    while (ftell(fp) < end) {
        char          id[5];
        unsigned long sz;
        int           rc = 0;

        if (rdid(fp, id) < 0)
            return fail("truncated pdta chunk");
        sz = rd32(fp);

        if      (strcmp(id, "phdr") == 0) rc = read_phdr(sf, sz, fp);
        else if (strcmp(id, "inst") == 0) rc = read_inst(sf, sz, fp);
        else if (strcmp(id, "shdr") == 0) rc = read_shdr(sf, sz, fp);
        else if (strcmp(id, "pbag") == 0)
            rc = read_bags(&sf->pbags, &sf->npbags, sz, fp,
                           "pbag size is not a multiple of 4");
        else if (strcmp(id, "ibag") == 0)
            rc = read_bags(&sf->ibags, &sf->nibags, sz, fp,
                           "ibag size is not a multiple of 4");
        else if (strcmp(id, "pgen") == 0)
            rc = read_gens(&sf->pgens, &sf->npgens, sz, fp,
                           "pgen size is not a multiple of 4");
        else if (strcmp(id, "igen") == 0)
            rc = read_gens(&sf->igens, &sf->nigens, sz, fp,
                           "igen size is not a multiple of 4");
        else if (strcmp(id, "pmod") == 0)
            rc = read_mods(&sf->pmods, &sf->npmods, sz, fp,
                           "pmod size is not a multiple of 10");
        else if (strcmp(id, "imod") == 0)
            rc = read_mods(&sf->imods, &sf->nimods, sz, fp,
                           "imod size is not a multiple of 10");
        else {
            /* An unknown sub-chunk is not an error - the format is
             * extensible and a reader that refuses one is wrong. */
            if (skip(fp, (long) sz) < 0)
                return fail("seek failed in pdta");
        }

        if (rc < 0)
            return rc;
        if (sz & 1) {
            if (skip(fp, 1L) < 0)
                return fail("seek failed on a RIFF pad byte");
        }
    }
    return 0;
}

int
sf2_load(sf2_file *sf, FILE *fp)
{
    char id[5];

    /* ZEROED FIRST, so sf2_free is safe however this returns. */
    memset(sf, 0, sizeof(*sf));
    sf->sample_pos = -1;

    if (rdid(fp, id) < 0 || strcmp(id, "RIFF") != 0)
        return fail("not a RIFF file");
    (void) rd32(fp);                    /* the total size; unused */
    if (rdid(fp, id) < 0 || strcmp(id, "sfbk") != 0)
        return fail("RIFF file is not a SoundFont (form is not sfbk)");

    for (;;) {
        unsigned long sz;
        char          lid[5];
        int           rc = 0;

        if (rdid(fp, id) < 0)
            break;                      /* clean EOF */
        sz = rd32(fp);
        if (strcmp(id, "LIST") != 0) {
            if (skip(fp, (long) sz) < 0)
                return fail("seek failed at top level");
            if (sz & 1) {
                if (skip(fp, 1L) < 0)
                    return fail("seek failed on a RIFF pad byte");
            }
            continue;
        }

        if (rdid(fp, lid) < 0)
            return fail("truncated LIST");

        if      (strcmp(lid, "INFO") == 0) rc = read_info(sf, sz, fp);
        else if (strcmp(lid, "sdta") == 0) rc = read_sdta(sf, sz, fp);
        else if (strcmp(lid, "pdta") == 0) rc = read_pdta(sf, sz, fp);
        else if (skip(fp, (long) sz - 4) < 0)
            return fail("seek failed skipping a LIST");

        if (rc < 0)
            return rc;
    }

    if (sf->presets == NULL)
        return fail("no phdr chunk - not a usable SoundFont");

    /*
     * REJECT SBK RATHER THAN HALF-READ IT. SoundFont 1.x uses 16-byte
     * shdr records and keeps sample names elsewhere, so a version-1
     * file that reached here would have been parsed with the wrong
     * record size throughout - producing plausible nonsense rather
     * than an error. TiMidity supports both (its sffile.c:626-630);
     * we do not, and say so.
     */
    if (sf->version_major < 2)
        return fail("SoundFont 1.x (SBK) is not supported");

    return 0;
}

void
sf2_free(sf2_file *sf)
{
    /* free(NULL) is defined, but the pointers are cleared so a double
     * call is safe too - a caller that frees on an error path and
     * again at exit is doing the right thing. */
    free(sf->presets); sf->presets = NULL;
    free(sf->insts);   sf->insts   = NULL;
    free(sf->samples); sf->samples = NULL;
    free(sf->pbags);   sf->pbags   = NULL;
    free(sf->ibags);   sf->ibags   = NULL;
    free(sf->pgens);   sf->pgens   = NULL;
    free(sf->igens);   sf->igens   = NULL;
    free(sf->pmods);   sf->pmods   = NULL;
    free(sf->imods);   sf->imods   = NULL;
    sf->npresets = sf->ninsts = sf->nsamples = 0;
    sf->npbags = sf->nibags = sf->npgens = sf->nigens = 0;
    sf->npmods = sf->nimods = 0;
}
