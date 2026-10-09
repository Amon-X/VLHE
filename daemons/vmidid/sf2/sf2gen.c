/*
 * sf2gen.c - the zone walk. See sf2gen.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * C89, gcc 2.95.2 clean. Written from the SF2.01 specification;
 * awesfx and TiMidity are used as oracles, not copied.
 */

#include <stdio.h>
#include <string.h>

#include "sf2gen.h"

/*
 * THE SPEC'S DEFAULTS, for generators absent from a zone.
 *
 * MOST ARE ZERO AND THREE ARE NOT, which is the whole reason this
 * table exists rather than a memset:
 *
 *   initialFilterFc  13500  - cents, ~19912 Hz, i.e. filter WIDE OPEN.
 *                             Defaulting it to 0 puts the cutoff at
 *                             8.176 Hz and every note goes silent.
 *   delay/attack/hold/decay/release  -12000 - timecents, ~1 ms. Zero
 *                             would mean 1 SECOND, so every note would
 *                             fade in.
 *   scaleTuning        100  - percent, i.e. normal equal temperament.
 *                             Zero would make every key play the same
 *                             pitch.
 *
 * sustain levels default to 0, which is correct: 0 centibels of
 * attenuation is FULL volume, not silence.
 */
static short
gen_default_of(int g)
{
    switch (g) {
    case SF2_initialFilterFc:  return 13500;
    case SF2_delayModLFO:
    case SF2_delayVibLFO:
    case SF2_delayModEnv:
    case SF2_attackModEnv:
    case SF2_holdModEnv:
    case SF2_decayModEnv:
    case SF2_releaseModEnv:
    case SF2_delayVolEnv:
    case SF2_attackVolEnv:
    case SF2_holdVolEnv:
    case SF2_decayVolEnv:
    case SF2_releaseVolEnv:    return -12000;
    case SF2_freqModLFO:
    case SF2_freqVibLFO:       return 0;
    case SF2_scaleTuning:      return 100;
    case SF2_overridingRootKey:
    case SF2_keynum:
    case SF2_velocity:         return -1;   /* "not set" per the spec */
    default:                   return 0;
    }
}

short
sf2_gen_default(int g)
{
    if (g < 0 || g >= SF2_GEN_COUNT)
        return 0;
    return gen_default_of(g);
}

static const char *gen_names[SF2_GEN_COUNT] = {
    "startAddrsOffset",       "endAddrsOffset",
    "startloopAddrsOffset",   "endloopAddrsOffset",
    "startAddrsCoarseOffset", "modLfoToPitch",
    "vibLfoToPitch",          "modEnvToPitch",
    "initialFilterFc",        "initialFilterQ",
    "modLfoToFilterFc",       "modEnvToFilterFc",
    "endAddrsCoarseOffset",   "modLfoToVolume",
    "unused1",                "chorusEffectsSend",
    "reverbEffectsSend",      "pan",
    "unused2",                "unused3",
    "unused4",                "delayModLFO",
    "freqModLFO",             "delayVibLFO",
    "freqVibLFO",             "delayModEnv",
    "attackModEnv",           "holdModEnv",
    "decayModEnv",            "sustainModEnv",
    "releaseModEnv",          "keynumToModEnvHold",
    "keynumToModEnvDecay",    "delayVolEnv",
    "attackVolEnv",           "holdVolEnv",
    "decayVolEnv",            "sustainVolEnv",
    "releaseVolEnv",          "keynumToVolEnvHold",
    "keynumToVolEnvDecay",    "instrument",
    "reserved1",              "keyRange",
    "velRange",               "startloopAddrsCoarse",
    "keynum",                 "velocity",
    "initialAttenuation",     "reserved2",
    "endloopAddrsCoarse",     "coarseTune",
    "fineTune",               "sampleID",
    "sampleModes",            "reserved3",
    "scaleTuning",            "exclusiveClass",
    "overridingRootKey",      "unused5"
};

const char *
sf2_gen_name(int g)
{
    if (g < 0 || g >= SF2_GEN_COUNT || gen_names[g] == NULL)
        return "unknown";
    return gen_names[g];
}

/*
 * WHAT THIS CODE ACTUALLY ACTS ON.
 *
 * A generator listed here is resolved and handed to the caller with a
 * meaning. Anything else is still READ, still stored and still
 * reported by sf2_zone_warn - but nothing downstream uses it yet, and
 * saying so at load time is the point.
 *
 * The effects sends are the honest case for "carried but unused":
 * there is no chorus or reverb unit for them to drive. They are named
 * anyway, because a SoundFont that leans on them WILL sound different
 * and the user deserves to be told which.
 *
 * THE FILTER GENERATORS WERE LISTED HERE AND SHOULD NOT HAVE BEEN,
 * corrected 2026-09-04. initialFilterFc and initialFilterQ were
 * resolved by the zone walk and then ignored by the renderer, which
 * has no filter - so this table CLAIMED them and the load-time
 * warning stayed silent about them.
 *
 * That is worse than not implementing them. The whole value of this
 * table is that a user is told what will not be honoured; an entry
 * that lies costs more than the feature it pretends to have. A
 * SoundFont whose character comes from its filter settings sounds
 * wrong, and the one mechanism meant to say so says nothing.
 */
static int
is_implemented(int g)
{
    switch (g) {
    case SF2_startAddrsOffset:      case SF2_endAddrsOffset:
    case SF2_startloopAddrsOffset:  case SF2_endloopAddrsOffset:
    case SF2_startAddrsCoarseOffset:case SF2_endAddrsCoarseOffset:
    case SF2_startloopAddrsCoarse:  case SF2_endloopAddrsCoarse:
    case SF2_pan:
    case SF2_delayVolEnv:           case SF2_attackVolEnv:
    case SF2_holdVolEnv:            case SF2_decayVolEnv:
    case SF2_sustainVolEnv:         case SF2_releaseVolEnv:
    case SF2_instrument:            case SF2_keyRange:
    case SF2_velRange:              case SF2_keynum:
    case SF2_velocity:              case SF2_initialAttenuation:
    case SF2_coarseTune:            case SF2_fineTune:
    case SF2_sampleID:              case SF2_sampleModes:
    case SF2_scaleTuning:           case SF2_exclusiveClass:
    case SF2_overridingRootKey:
    /* The LFOs and the modulation envelope, added 2026-09-04. The
     * filter destinations among them (modLfoToFilterFc,
     * modEnvToFilterFc) are deliberately NOT here - there is still no
     * filter for them to sweep. */
    case SF2_modLfoToPitch:         case SF2_vibLfoToPitch:
    case SF2_modLfoToVolume:
    /*
     * The keynum-to-envelope scalers, added 2026-09-04 with the
     * generators themselves - see keynum_scaled_tc in render.c. Hold
     * and decay track the keyboard, pivoting at key 60.
     */
    case SF2_keynumToVolEnvHold:    case SF2_keynumToVolEnvDecay:
    case SF2_keynumToModEnvHold:    case SF2_keynumToModEnvDecay:
    /* The resonant low-pass and its two modulation sources, added
     * 2026-09-04. These were listed here once before while nothing
     * filtered - see the note above - and are now genuinely done. */
    case SF2_initialFilterFc:       case SF2_initialFilterQ:
    case SF2_modLfoToFilterFc:      case SF2_modEnvToFilterFc:
    case SF2_delayModLFO:           case SF2_freqModLFO:
    case SF2_delayVibLFO:           case SF2_freqVibLFO:
    case SF2_modEnvToPitch:
    case SF2_delayModEnv:           case SF2_attackModEnv:
    case SF2_holdModEnv:            case SF2_decayModEnv:
    case SF2_sustainModEnv:         case SF2_releaseModEnv:
    /*
     * THE TWO EFFECTS SENDS - added 2026-09-29, and they were the
     * last two generators this function called unimplemented.
     *
     * THEY ARE ACTED ON, AND HAVE BEEN SINCE THE EFFECTS LANDED.
     * `render.c:1492' takes both through `voice_set_sends()', which
     * scales them by the voice gain into `send_reverb_gain' and
     * `send_chorus_gain'; `:3650-3654' accumulates each voice into
     * the reverb and chorus buses. `design/21' designed the buses
     * and its steps 1-4 built them.
     *
     * SO THE WARNING WAS LYING. `sf2_zone_warn()' told a user
     * loading the SC-55 that its reverb and chorus sends were
     * ignored while the renderer was applying them - which is worse
     * than saying nothing, because the whole point of that function
     * (design/p3-midi-plan section 8) is that ignoring a generator
     * is not honest.
     *
     * "IMPLEMENTED" HERE MEANS "WHEN EFFECTS ARE ON". Both sends are
     * gated on `r->effects' (`render.c:3316'), so under `-E off'
     * they genuinely are ignored - and this function is static, with
     * no access to that flag. The choice is deliberate: a font's
     * generator is either something we understand or it is not, and
     * a user who turned effects off already knows the effects are
     * off. The alternative - a warning that appears only under
     * `-E off' - would report a setting the user chose as though the
     * FONT were at fault.
     */
    case SF2_reverbEffectsSend:     case SF2_chorusEffectsSend:
        return 1;
    default:
        return 0;
    }
}

/*
 * A RANGE GENERATOR PACKS TWO BYTES INTO ONE SHORT: low in the least
 * significant byte, high in the most. Reading it as a scalar gives a
 * number in the thousands and every note fails to match.
 */
static int
range_lo(unsigned short amount)
{
    return (int) (amount & 0xff);
}

static int
range_hi(unsigned short amount)
{
    return (int) ((amount >> 8) & 0xff);
}

static int
in_range(unsigned short amount, int v)
{
    return v >= range_lo(amount) && v <= range_hi(amount);
}

/*
 * Does this zone accept the note? A zone with no keyRange or velRange
 * accepts everything, which is what the spec says and what a
 * single-zone instrument relies on.
 */
static int
zone_matches(const sf2_gen *gens, int first, int last, int key, int vel)
{
    int i;
    int ok = 1;

    for (i = first; i < last; i++) {
        if (gens[i].oper == SF2_keyRange && !in_range(gens[i].amount, key))
            ok = 0;
        else if (gens[i].oper == SF2_velRange && !in_range(gens[i].amount, vel))
            ok = 0;
    }
    return ok;
}

/* Is a terminating generator present? Its absence marks a global zone. */
static int
has_gen(const sf2_gen *gens, int first, int last, int which)
{
    int i;
    for (i = first; i < last; i++)
        if (gens[i].oper == which)
            return 1;
    return 0;
}

static int
gen_value(const sf2_gen *gens, int first, int last, int which, int *found)
{
    int i;
    /* THE LAST OCCURRENCE WINS. The spec allows a generator to appear
     * more than once in a zone and says the final one is effective. */
    *found = 0;
    for (i = first; i < last; i++)
        if (gens[i].oper == which) {
            *found = 1;
            return (int) (short) gens[i].amount;
        }
    return 0;
}

/* Apply a zone's generators absolutely - the instrument rule. */
static void
apply_absolute(sf2_zone *z, const sf2_gen *gens, int first, int last)
{
    int i;
    for (i = first; i < last; i++) {
        int g = (int) gens[i].oper;
        if (g < 0 || g >= SF2_GEN_COUNT)
            continue;
        z->gen[g] = (short) gens[i].amount;
        z->set[g] = 1;
    }
}

/*
 * Apply a zone's generators additively - the PRESET rule, and the one
 * that is easy to get wrong.
 *
 * A preset generator OFFSETS what the instrument decided. Setting it
 * instead discards the instrument's own tuning, attenuation and
 * envelopes, which is the classic SF2 bug: the note plays, at roughly
 * the right pitch, sounding wrong in a way that is hard to attribute.
 *
 * THE RANGES AND THE INDICES ARE NOT ADDITIVE. keyRange and velRange
 * are filters, already applied to choose this zone; instrument and
 * sampleID are indices, and adding two of them is meaningless.
 */
static void
apply_additive(sf2_zone *z, const sf2_gen *gens, int first, int last)
{
    int i;
    for (i = first; i < last; i++) {
        int g = (int) gens[i].oper;
        if (g < 0 || g >= SF2_GEN_COUNT)
            continue;
        if (g == SF2_keyRange || g == SF2_velRange ||
            g == SF2_instrument || g == SF2_sampleID)
            continue;
        z->gen[g] = (short) (z->gen[g] + (short) gens[i].amount);
        z->set[g] = 1;
    }
}

/*
 * EVERY zone matching this note, not just the first.
 *
 * A stereo sample is stored as two instrument zones over the same key
 * range, panned hard left and hard right, and both must sound. This
 * returned on the first match until 2026-09-04, which put every
 * stereo instrument in the left speaker - measured at 64.5 dB of
 * channel imbalance on the SC-55's Choir Aahs against 1.7 dB from
 * fluidsynth and 1.3 dB from TiMidity.
 */
int
sf2_zone_find_all(const sf2_file *sf, int preset_index, int key, int vel,
                  sf2_zone *out, int max)
{
    return sf2_zone_find_all_m(sf, preset_index, key, vel, out, max, 0);
}

int
sf2_zone_find_all_m(const sf2_file *sf, int preset_index, int key, int vel,
                    sf2_zone *out, int max, unsigned modflags)
{
    int pz, pz_first, pz_last;
    int pglobal_first = 0, pglobal_last = 0;
    /* The modulator ranges walk beside the generator ranges: a bag
     * indexes both, and the global zone's are captured at the same
     * moment its generators are. */
    int pmglobal_first = 0, pmglobal_last = 0;
    int nfound = 0;

    if (sf == NULL || out == NULL || max < 1)
        return -1;
    if (preset_index < 0 || preset_index >= sf->npresets)
        return -1;

    /*
     * THE TERMINAL RECORD IS WHAT MAKES THIS WORK. presets[i+1] is
     * always readable - sf2.c keeps the array at full on-disk length
     * precisely so that the last real preset has an end.
     */
    pz_first = (int) sf->presets[preset_index].bag_index;
    pz_last  = (int) sf->presets[preset_index + 1].bag_index;

    for (pz = pz_first; pz < pz_last; pz++) {
        int pg_first, pg_last, inst, found;
        int pm_first, pm_last;
        int iz, iz_first, iz_last;
        int iglobal_first = 0, iglobal_last = 0;
        int imglobal_first = 0, imglobal_last = 0;

        if (pz < 0 || pz + 1 > sf->npbags)
            return -1;
        pg_first = (int) sf->pbags[pz].gen_index;
        pg_last  = (int) sf->pbags[pz + 1].gen_index;
        if (pg_first > pg_last || pg_last > sf->npgens)
            return -1;
        pm_first = (int) sf->pbags[pz].mod_index;
        pm_last  = (int) sf->pbags[pz + 1].mod_index;
        if (pm_first > pm_last || pm_last > sf->npmods)
            return -1;

        /*
         * A LEADING ZONE WITHOUT `instrument' IS THE GLOBAL ZONE.
         * Its generators are defaults for every later zone in this
         * preset. Real files use it: preset 0 of the SC-55 has one
         * carrying nothing but initialAttenuation.
         */
        if (!has_gen(sf->pgens, pg_first, pg_last, SF2_instrument)) {
            if (pz == pz_first) {
                pglobal_first  = pg_first;
                pglobal_last   = pg_last;
                pmglobal_first = pm_first;
                pmglobal_last  = pm_last;
            }
            continue;               /* never a playable zone itself */
        }

        if (!zone_matches(sf->pgens, pg_first, pg_last, key, vel))
            continue;

        inst = gen_value(sf->pgens, pg_first, pg_last, SF2_instrument,
                         &found);
        if (!found || inst < 0 || inst >= sf->ninsts)
            continue;

        iz_first = (int) sf->insts[inst].bag_index;
        iz_last  = (int) sf->insts[inst + 1].bag_index;

        for (iz = iz_first; iz < iz_last; iz++) {
            int ig_first, ig_last, smp;
            int im_first, im_last;

            if (iz < 0 || iz + 1 > sf->nibags)
                return -1;
            ig_first = (int) sf->ibags[iz].gen_index;
            ig_last  = (int) sf->ibags[iz + 1].gen_index;
            if (ig_first > ig_last || ig_last > sf->nigens)
                return -1;
            im_first = (int) sf->ibags[iz].mod_index;
            im_last  = (int) sf->ibags[iz + 1].mod_index;
            if (im_first > im_last || im_last > sf->nimods)
                return -1;

            if (!has_gen(sf->igens, ig_first, ig_last, SF2_sampleID)) {
                if (iz == iz_first) {
                    iglobal_first  = ig_first;
                    iglobal_last   = ig_last;
                    imglobal_first = im_first;
                    imglobal_last  = im_last;
                }
                continue;
            }

            if (!zone_matches(sf->igens, ig_first, ig_last, key, vel))
                continue;

            smp = gen_value(sf->igens, ig_first, ig_last, SF2_sampleID,
                            &found);
            if (!found || smp < 0 || smp >= sf->nsamples)
                continue;

            /*
             * THE MERGE, AND THE ORDER IS THE SEMANTICS.
             *
             * Instrument first and absolute: global zone, then this
             * zone overriding it. Then preset, additive, in the same
             * order. Reversing any of these four gives a note that
             * plays and sounds wrong.
             */
            {
                sf2_zone *z = &out[nfound];
                int g;

                for (g = 0; g < SF2_GEN_COUNT; g++) {
                    z->gen[g] = gen_default_of(g);
                    z->set[g] = 0;
                }
                z->sample_index = -1;

                apply_absolute(z, sf->igens, iglobal_first, iglobal_last);
                apply_absolute(z, sf->igens, ig_first, ig_last);
                apply_additive(z, sf->pgens, pglobal_first, pglobal_last);
                apply_additive(z, sf->pgens, pg_first, pg_last);

                z->sample_index = smp;

                /* THE MODULATORS, same four levels, their own rules:
                 * instrument supersedes, preset adds. sf2mod.h. */
                sf2_mod_merge(&z->mods,
                              sf->imods, imglobal_first, imglobal_last,
                                         im_first, im_last,
                              sf->pmods, pmglobal_first, pmglobal_last,
                                         pm_first, pm_last,
                              modflags);
            }

            /*
             * KEEP LOOKING. Returning here is what put a stereo
             * instrument in one speaker - see the header. Every zone
             * matching this key and velocity sounds.
             */
            if (++nfound >= max)
                return nfound;
        }
    }

    return nfound;                  /* 0 = nothing mapped here */
}

/*
 * The first zone only, for callers that genuinely want one.
 */
int
sf2_zone_find(const sf2_file *sf, int preset_index,
              int key, int vel, sf2_zone *out)
{
    int n = sf2_zone_find_all(sf, preset_index, key, vel, out, 1);
    return n < 0 ? n : (n > 0 ? 1 : 0);
}

int
sf2_zone_warn(const sf2_file *sf,
              void (*cb)(int gen, const char *name, int count, void *arg),
              void *arg)
{
    int counts[SF2_GEN_COUNT];
    int i, distinct = 0;

    if (sf == NULL)
        return 0;

    for (i = 0; i < SF2_GEN_COUNT; i++)
        counts[i] = 0;

    for (i = 0; i < sf->nigens; i++) {
        int g = (int) sf->igens[i].oper;
        if (g >= 0 && g < SF2_GEN_COUNT && !is_implemented(g))
            counts[g]++;
    }
    for (i = 0; i < sf->npgens; i++) {
        int g = (int) sf->pgens[i].oper;
        if (g >= 0 && g < SF2_GEN_COUNT && !is_implemented(g))
            counts[g]++;
    }

    for (i = 0; i < SF2_GEN_COUNT; i++) {
        if (counts[i] == 0)
            continue;
        distinct++;
        if (cb != NULL)
            cb(i, sf2_gen_name(i), counts[i], arg);
    }
    return distinct;
}
