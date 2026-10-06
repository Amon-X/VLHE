/*
 * sf2mod.c - the SF2.01 modulator defaults and the 9.5.1 merge.
 * See sf2mod.h. C89, gcc 2.95.2 clean.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 */

#include <string.h>

#include "sf2gen.h"             /* SF2_GEN_COUNT; includes sf2mod.h */

/* The source enumerator, SF2.01 8.2: index 0-6, C bit 7, D bit 8,
 * P bit 9, type bits 10-15. */
#define SRC_INDEX(e)   ((int) ((e) & 0x7f))
#define SRC_CC(e)      ((int) (((e) >> 7) & 1))
#define SRC_D(e)       ((int) (((e) >> 8) & 1))
#define SRC_P(e)       ((int) (((e) >> 9) & 1))
#define SRC_TYPE(e)    ((int) (((e) >> 10) & 0x3f))

#define TYPE_LINEAR    0
#define TYPE_CONCAVE   1
#define TYPE_CONVEX    2
#define TYPE_SWITCH    3

/*
 * THE TEN DEFAULTS, SF2.01 8.4.1 - 8.4.10, in the sf2_mod field order
 * (src, dest, amount, amt_src, transform). Every enumerator is the
 * spec's own except one:
 *
 * 8.4.2's amount source. The spec PRINTS it as "0x502 (type=3 ...)",
 * which cannot be - 0x0502 encodes type 1, concave; a switch on
 * velocity is 0x0D02, and that is what every font writes (8778 of
 * them). And this table carries it as 0x0C02, D=0, the flipped form
 * that sf2mod.h explains: a ramp above velocity 64 and nothing below,
 * the hardware's shape rather than the spec's jump. Whether a font's
 * 0x0D02 matches it is sf2_mod_is_velfilter()'s business, not the
 * identity test's.
 *
 * 8.4.6's AMOUNT IS 500, NOT THE 1000 THE SPEC PRINTS. Pan's own
 * range is -500..500 (tenths of a percent, 8.1.3 gen 17), so 1000
 * would pin CC10 to the rails at a quarter turn each way. fluidsynth
 * registers 500 (fluid_synth.c:457), and the spec test suite's own
 * font treats 500 as the default: its panning test F overrides the
 * modulator to 1000 and calls the instrument "2x-pan", and its README
 * says CC10 = 127 is "49.22%" - which is 500 x 63/64. Gus1live's 527
 * CC10 modulators are 1000 for the same reason: a deliberate double
 * width, which reads as an alteration only against a 500 default.
 *
 * 8.4.10's DESTINATION IS fineTune (52). The spec names "Initial
 * Pitch", which is not a numbered generator, so a font that wants to
 * cancel or invert pitch bend has to pick a number - and fluidsynth
 * (fluid_synth.c:517), Polyphone and the spec test suite (its pitch
 * bend instrument cancels and inverts at 52) all picked fineTune. So
 * does this table; anything else and the suite's test 20 B/C could
 * never match.
 */
static const sf2_mod defaults[SF2_MOD_NDEFAULTS] = {
    { 0x0502, SF2_initialAttenuation,   960, 0x0000, 0 }, /* 8.4.1  */
    { 0x0102, SF2_initialFilterFc,    -2400, 0x0C02, 0 }, /* 8.4.2  */
    { 0x000D, SF2_vibLfoToPitch,         50, 0x0000, 0 }, /* 8.4.3  */
    { 0x0081, SF2_vibLfoToPitch,         50, 0x0000, 0 }, /* 8.4.4  */
    { 0x0587, SF2_initialAttenuation,   960, 0x0000, 0 }, /* 8.4.5  */
    { 0x028A, SF2_pan,                  500, 0x0000, 0 }, /* 8.4.6  */
    { 0x058B, SF2_initialAttenuation,   960, 0x0000, 0 }, /* 8.4.7  */
    { 0x00DB, SF2_reverbEffectsSend,    200, 0x0000, 0 }, /* 8.4.8  */
    { 0x00DD, SF2_chorusEffectsSend,    200, 0x0000, 0 }, /* 8.4.9  */
    { 0x020E, SF2_fineTune,           12700, 0x0010, 0 }  /* 8.4.10 */
};

#define VELFILTER_INDEX  1              /* defaults[1] is 8.4.2 */

/* The three forms 8.4.2 can take - sf2mod.h has the four modes. */
static const sf2_mod vf_awe = { 0x0102, SF2_initialFilterFc, -2400, 0x0C02, 0 };
static const sf2_mod vf_201 = { 0x0102, SF2_initialFilterFc, -2400, 0x0D02, 0 };
static const sf2_mod vf_204 = { 0x0102, SF2_initialFilterFc, -2400, 0x0000, 0 };

const sf2_mod *
sf2_mod_defaults(void)
{
    return defaults;
}

const sf2_mod *
sf2_mod_velfilter_row(unsigned flags)
{
    switch (flags & SF2_MOD_VF_MASK) {
    case SF2_MOD_VF_AWE:  return &vf_awe;
    case SF2_MOD_VF_201:  return &vf_201;
    case SF2_MOD_VF_204:  return &vf_204;
    default:              return NULL;      /* SF2_MOD_VF_NONE */
    }
}

int
sf2_mod_vf_parse(const char *s)
{
    if (s == NULL)
        return -1;
    if (!strcmp(s, "1") || !strcmp(s, "awe"))  return SF2_MOD_VF_AWE;
    if (!strcmp(s, "0") || !strcmp(s, "none")) return SF2_MOD_VF_NONE;
    if (!strcmp(s, "2") || !strcmp(s, "2.01")) return SF2_MOD_VF_201;
    if (!strcmp(s, "3") || !strcmp(s, "2.04")) return SF2_MOD_VF_204;
    return -1;
}

const char *
sf2_mod_vf_name(unsigned flags)
{
    switch (flags & SF2_MOD_VF_MASK) {
    case SF2_MOD_VF_AWE:  return "awe";
    case SF2_MOD_VF_201:  return "2.01";
    case SF2_MOD_VF_204:  return "2.04";
    default:              return "none";
    }
}

int
sf2_mod_law_parse(const char *s)
{
    if (s == NULL)
        return -1;
    if (!strcmp(s, "0") || !strcmp(s, "spec"))   return SF2_MOD_LAW_SPEC;
    if (!strcmp(s, "1") || !strcmp(s, "linear")) return SF2_MOD_LAW_LINEAR;
    return -1;
}

const char *
sf2_mod_law_name(unsigned flags)
{
    return (flags & SF2_MOD_LAW_MASK) == SF2_MOD_LAW_LINEAR ? "linear" : "spec";
}

const char *
sf2_mod_law_desc(unsigned flags)
{
    /*
     * FOR A HUMAN, where sf2_mod_law_name() is for matching a flag.
     *
     * THE BARE NAMES CONFUSE TWO OPTIONS INTO ONE. A banner reading
     * "velocity-filter default: awe, law linear" looks like a single
     * self-contradicting setting, because -F's value and -L's value
     * sit adjacent and AWE appears in BOTH: it is a -F mode, it is
     * where -L spec's curve comes from, and it is where -L linear's
     * comes from too. Three things, one word. The user hit it
     * 2026-09-18: "awe and linear is confusing".
     *
     * design/09 had already recorded the underlying problem -
     * "linear is true of the arithmetic and says nothing to a user" -
     * and the GUI solved it: vlhe_mod_midi.c labels the control
     * "Volume curve" and offers "SoundFont standard (spec)" and
     * "Gentler - Sound Blaster era (linear)". This is that wording,
     * brought back to the command line so a log line reads the way
     * the GUI does.
     */
    return (flags & SF2_MOD_LAW_MASK) == SF2_MOD_LAW_LINEAR
           ? "linear (gentler, Sound Blaster era)"
           : "spec (SoundFont standard)";
}

int
sf2_mod_identical(const sf2_mod *a, const sf2_mod *b)
{
    return a->src       == b->src
        && a->dest      == b->dest
        && a->amt_src   == b->amt_src
        && a->transform == b->transform;
}

/*
 * 8.4.2's SHAPE: velocity, linear, unipolar, negative, into
 * initialFilterFc, with a velocity switch (unipolar, EITHER
 * direction) as the amount source, linear transform. The D bit of the
 * switch is exactly what is not compared - sf2mod.h says why.
 */
int
sf2_mod_is_velfilter(const sf2_mod *m)
{
    return m->src == 0x0102
        && m->dest == SF2_initialFilterFc
        && m->transform == 0
        && SRC_INDEX(m->amt_src) == 2
        && SRC_CC(m->amt_src) == 0
        && SRC_P(m->amt_src) == 0
        && SRC_TYPE(m->amt_src) == TYPE_SWITCH;
}

/*
 * 8.2.1: which sources exist. The general palette names seven; the
 * MIDI palette is any CC except the ones the spec lists as illegal
 * (MIDI functions rather than controllers) or reserved (LSB slots).
 * Index 127 is the 2.04 link source, unsupported here - "the entire
 * modulator structure should be ignored" for anything unknown.
 */
static int
source_ok(unsigned short e)
{
    int idx = SRC_INDEX(e);

    if (SRC_TYPE(e) > TYPE_SWITCH)
        return 0;
    if (SRC_CC(e)) {
        if (idx == 0 || idx == 6 || idx == 32 || idx == 38)
            return 0;
        if (idx >= 33 && idx <= 63)
            return 0;
        if (idx >= 98 && idx <= 101)
            return 0;
        if (idx >= 120)
            return 0;
        return 1;
    }
    switch (idx) {
    case 0: case 2: case 3: case 10: case 13: case 14: case 16:
        return 1;
    default:
        return 0;
    }
}

int
sf2_mod_valid(const sf2_mod *m)
{
    if (!source_ok(m->src) || !source_ok(m->amt_src))
        return 0;
    /* 2.01 defines transform 0 only; 2.04 adds 2 (absolute value).
     * Anything else is unknown and the modulator is ignored whole. */
    if (m->transform != 0 && m->transform != 2)
        return 0;
    /* Bit 15 set is a 2.04 link destination, unsupported. */
    if (m->dest & 0x8000)
        return 0;
    if (m->dest >= SF2_GEN_COUNT)
        return 0;
    return 1;
}

static int
find_identical(const sf2_modlist *ml, const sf2_mod *m)
{
    int i;
    for (i = 0; i < ml->n; i++)
        if (sf2_mod_identical(&ml->mod[i], m))
            return i;
    return -1;
}

/* Sum two amounts without leaving a short. */
static short
add_amount(short a, short b)
{
    long s = (long) a + (long) b;
    if (s >  32767L) s =  32767L;
    if (s < -32768L) s = -32768L;
    return (short) s;
}

/*
 * Put one modulator into a list: supersede an identical entry, or
 * add to it, or append. `add' is the preset rule (amounts sum); its
 * absence is the instrument rule (the later one replaces).
 */
static void
put(sf2_modlist *ml, const sf2_mod *m, int from, int zone, int add)
{
    int k = find_identical(ml, m);

    if (k >= 0) {
        /* Add: the preset rule, or an identical pair within ONE zone
         * (sf2mod.h). Otherwise the later place supersedes. */
        if (add || ml->zone[k] == (unsigned char) zone)
            ml->mod[k].amount = add_amount(ml->mod[k].amount, m->amount);
        else
            ml->mod[k].amount = m->amount;
        ml->from[k] = (unsigned char) from;
        ml->zone[k] = (unsigned char) zone;
        return;
    }
    if (ml->n >= SF2_MAX_MODS) {
        ml->dropped++;
        return;
    }
    ml->mod[ml->n]  = *m;
    ml->from[ml->n] = (unsigned char) from;
    ml->zone[ml->n] = (unsigned char) zone;
    ml->n++;
}

/*
 * Take one modulator from a file, applying validity and the
 * NORMALISE rule, then put it. Returns 0 if it was ignored.
 */
static int
take(sf2_modlist *ml, const sf2_mod *src, int from, int zone, int add,
     unsigned flags)
{
    sf2_mod m = *src;

    if (!sf2_mod_valid(&m)) {
        ml->dropped++;
        return 0;
    }
    if (sf2_mod_is_velfilter(&m)) {
        const sf2_mod *row = sf2_mod_velfilter_row(flags);

        if (row == NULL)
            return 0;               /* a statement about no default */
        m.amt_src = row->amt_src;   /* the active row's form */
    }
    put(ml, &m, from, zone, add);
    return 1;
}

void
sf2_mod_merge(sf2_modlist *ml,
              const sf2_mod *imods, int ig0, int ig1, int il0, int il1,
              const sf2_mod *pmods, int pg0, int pg1, int pl0, int pl1,
              unsigned flags)
{
    sf2_modlist pl;
    int i;

    memset(ml, 0, sizeof *ml);

    /* The defaults are the instrument level's starting point, with
     * 8.4.2 in whichever form the mode plays - or absent. */
    for (i = 0; i < SF2_MOD_NDEFAULTS; i++) {
        const sf2_mod *d = &defaults[i];
        sf2_mod lin;

        if (i == VELFILTER_INDEX) {
            d = sf2_mod_velfilter_row(flags);
            if (d == NULL)
                continue;
        }
        /* The linear law: the three attenuation defaults at half
         * amount, 480 cB - gain v/127 rather than (v/127)^2. See
         * SF2_MOD_LAW_LINEAR in the header. The row stays identical
         * to the spec's for supersede/add purposes, since identity
         * ignores the amount. */
        if ((flags & SF2_MOD_LAW_MASK) == SF2_MOD_LAW_LINEAR
            && d->dest == SF2_initialAttenuation) {
            lin = *d;
            lin.amount = 480;
            d = &lin;
        }
        put(ml, d, SF2_MOD_FROM_DEFAULT, SF2_MOD_ZONE_DEFAULT, 0);
    }

    /*
     * INSTRUMENT LEVEL: global then local, each SUPERSEDING an
     * identical entry (default or earlier zone) and adding otherwise.
     * "Superseding" is the later amount replacing the earlier one -
     * which is how amount 0 in a local zone switches a default off.
     */
    if (imods != NULL) {
        for (i = ig0; i < ig1; i++)
            take(ml, &imods[i], SF2_MOD_FROM_INST, SF2_MOD_ZONE_IGLOBAL, 0, flags);
        for (i = il0; i < il1; i++)
            take(ml, &imods[i], SF2_MOD_FROM_INST, SF2_MOD_ZONE_ILOCAL, 0, flags);
    }

    /*
     * PRESET LEVEL: resolved against ITSELF first (local supersedes
     * global, within the preset), and the result then ADDS to the
     * instrument level whether identical or not. Identical amounts
     * sum into the existing entry - the spec says that is exactly
     * equivalent to two modulators - so the list does not grow for a
     * preset restating an instrument's modulator.
     */
    memset(&pl, 0, sizeof pl);
    if (pmods != NULL) {
        for (i = pg0; i < pg1; i++)
            take(&pl, &pmods[i], SF2_MOD_FROM_PRESET, SF2_MOD_ZONE_PGLOBAL, 0, flags);
        for (i = pl0; i < pl1; i++)
            take(&pl, &pmods[i], SF2_MOD_FROM_PRESET, SF2_MOD_ZONE_PLOCAL, 0, flags);
    }
    for (i = 0; i < pl.n; i++)
        put(ml, &pl.mod[i], SF2_MOD_FROM_PRESET, pl.zone[i], 1);
    ml->dropped += pl.dropped;
}
