/*
 * sf2gen.h - SF2.01 generators, and the zone walk that resolves them.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * sf2.h reads the container into arrays that mirror the file. This
 * turns those arrays into an answer to the only question a synth
 * asks: "for note N at velocity V on this preset, what do I play?"
 *
 * THE WALK, which is the whole of it:
 *
 *     preset -> pbag -> pgen -> instrument -> ibag -> igen -> sample
 *
 * A preset zone selects an instrument; an instrument zone selects a
 * sample. Both may be filtered by key and velocity range, and both
 * may carry generators that shape the note.
 *
 * THE TWO RULES THAT MATTER, and they are not symmetric:
 *
 *   1. INSTRUMENT generators are ABSOLUTE - they set the value.
 *      PRESET generators are ADDITIVE - they offset whatever the
 *      instrument decided. Treating preset values as absolute is the
 *      classic SF2 bug; it silently ignores the instrument's own
 *      tuning and envelopes.
 *
 *   2. A leading zone with no `instrument' generator (preset side) or
 *      no `sampleID' (instrument side) is a GLOBAL ZONE: its values
 *      are defaults for every later zone in that preset or
 *      instrument. Real files use this - preset 0 of the SC-55 has a
 *      global zone carrying nothing but initialAttenuation.
 *
 * WHAT IS IMPLEMENTED. All 60 spec generators are RECOGNISED, stored
 * and reported. Those with no target in a sampler with no effects
 * engine (chorus, reverb) are carried but unused, and sf2_zone_warn
 * names anything found that this code does not act on - at LOAD time,
 * by name, rather than dropping it silently.
 *
 * That distinction is design/p3-midi-plan.md section 8, and it is the
 * user's own correction: ignoring a MIDI event is honest, because the
 * note still plays correctly. Ignoring an SF2 generator is not - the
 * note plays WRONG, and a user hears "vmidi sounds bad" rather than
 * "vmidi does not support that".
 */

#ifndef SF2GEN_H
#define SF2GEN_H

#include "sf2.h"
#include "sf2mod.h"

/*
 * THE GENERATOR NUMBERS ARE THE SPECIFICATION'S, 0 to 59.
 *
 * Confirmed against real files rather than copied from a header: in
 * Roland.SC-55.sf2, generator 41 appears only in pgen and 53 only in
 * igen, which is exactly what the spec requires of instrument and
 * sampleID.
 */
enum {
    SF2_startAddrsOffset        = 0,
    SF2_endAddrsOffset          = 1,
    SF2_startloopAddrsOffset    = 2,
    SF2_endloopAddrsOffset      = 3,
    SF2_startAddrsCoarseOffset  = 4,
    SF2_modLfoToPitch           = 5,
    SF2_vibLfoToPitch           = 6,
    SF2_modEnvToPitch           = 7,
    SF2_initialFilterFc         = 8,
    SF2_initialFilterQ          = 9,
    SF2_modLfoToFilterFc        = 10,
    SF2_modEnvToFilterFc        = 11,
    SF2_endAddrsCoarseOffset    = 12,
    SF2_modLfoToVolume          = 13,
    SF2_unused1                 = 14,
    SF2_chorusEffectsSend       = 15,
    SF2_reverbEffectsSend       = 16,
    SF2_pan                     = 17,
    SF2_unused2                 = 18,
    SF2_unused3                 = 19,
    SF2_unused4                 = 20,
    SF2_delayModLFO             = 21,
    SF2_freqModLFO              = 22,
    SF2_delayVibLFO             = 23,
    SF2_freqVibLFO              = 24,
    SF2_delayModEnv             = 25,
    SF2_attackModEnv            = 26,
    SF2_holdModEnv              = 27,
    SF2_decayModEnv             = 28,
    SF2_sustainModEnv           = 29,
    SF2_releaseModEnv           = 30,
    SF2_keynumToModEnvHold      = 31,
    SF2_keynumToModEnvDecay     = 32,
    SF2_delayVolEnv             = 33,
    SF2_attackVolEnv            = 34,
    SF2_holdVolEnv              = 35,
    SF2_decayVolEnv             = 36,
    SF2_sustainVolEnv           = 37,
    SF2_releaseVolEnv           = 38,
    SF2_keynumToVolEnvHold      = 39,
    SF2_keynumToVolEnvDecay     = 40,
    SF2_instrument              = 41,
    SF2_reserved1               = 42,
    SF2_keyRange                = 43,
    SF2_velRange                = 44,
    SF2_startloopAddrsCoarse    = 45,
    SF2_keynum                  = 46,
    SF2_velocity                = 47,
    SF2_initialAttenuation      = 48,
    SF2_reserved2               = 49,
    SF2_endloopAddrsCoarse      = 50,
    SF2_coarseTune              = 51,
    SF2_fineTune                = 52,
    SF2_sampleID                = 53,
    SF2_sampleModes             = 54,
    SF2_reserved3               = 55,
    SF2_scaleTuning             = 56,
    SF2_exclusiveClass          = 57,
    SF2_overridingRootKey       = 58,
    SF2_unused5                 = 59,
    SF2_GEN_COUNT               = 60
};

/* sampleModes (54) is a bit field, not a scalar. */
#define SF2_LOOP_NONE      0
#define SF2_LOOP_CONTINUOUS 1
#define SF2_LOOP_UNUSED    2
#define SF2_LOOP_TO_END    3

/*
 * A fully resolved zone: what to play for one note.
 *
 * `gen' holds every generator's value after the instrument-absolute
 * then preset-additive merge. `set' records which were present in the
 * file at all, so a caller can tell "explicitly zero" from "defaulted
 * to zero" - which matters for pan and tuning.
 */
typedef struct {
    short gen[SF2_GEN_COUNT];
    char  set[SF2_GEN_COUNT];
    int   sample_index;         /* into sf2_file.samples, -1 if none */

    /*
     * THE ZONE'S MODULATORS, merged per SF2.01 9.5.1 - the defaults,
     * then the instrument's (superseding), then the preset's
     * (adding). sf2mod.h. One entry per distinct modulator, amounts
     * already combined; the renderer evaluates each at note-on.
     */
    sf2_modlist mods;
} sf2_zone;

/*
 * A NOTE CAN MATCH MORE THAN ONE ZONE, and that is how SF2 stores a
 * stereo sample: two instrument zones over the same key range, one
 * panned hard left and one hard right, both sounded together. The
 * SC-55's "Choir Aahs" has exactly that at key 60 - zones at pan -500
 * and +500 - and playing only the first put the whole instrument in
 * the left speaker.
 *
 * SF2_MAX_ZONES is the cap. Two is the common case (stereo); the spec
 * sets no limit, and a soundfont that layers three or four samples on
 * one key is legal and not rare. Eight is well past anything observed
 * and bounds the caller's voice budget.
 */
#define SF2_MAX_ZONES 8

/*
 * Find EVERY zone for one note. Returns how many were written to
 * `out' (0 when the preset has nothing mapped at that key and
 * velocity), or negative on a malformed file. At most `max'.
 *
 * `preset_index' is into sf2_file.presets. `key' and `vel' are 0-127.
 */
int sf2_zone_find_all(const sf2_file *sf, int preset_index,
                      int key, int vel, sf2_zone *out, int max);

/*
 * The same, with flags for the modulator merge (SF2_MOD_* in
 * sf2mod.h). sf2_zone_find_all() is this with flags 0 - the default
 * velocity-filter modulator in. A synth with a policy passes it here.
 */
int sf2_zone_find_all_m(const sf2_file *sf, int preset_index,
                        int key, int vel, sf2_zone *out, int max,
                        unsigned modflags);

/*
 * The first zone only. Returns 1 when one was found, 0 when none,
 * negative on a malformed file.
 *
 * KEPT FOR CALLERS THAT GENUINELY WANT ONE - the load-time survey in
 * smfdump, and tests. A synth must use sf2_zone_find_all, or a stereo
 * instrument plays half of itself.
 */
int sf2_zone_find(const sf2_file *sf, int preset_index,
                  int key, int vel, sf2_zone *out);

/*
 * Every generator this file uses, walked once, reporting any that are
 * present but not acted on. Call it at LOAD time.
 *
 * `cb' is called once per unimplemented generator with its number,
 * its name, and how many times it occurred. Pass NULL to count only.
 * Returns the number of DISTINCT unimplemented generators found.
 */
int sf2_zone_warn(const sf2_file *sf,
                  void (*cb)(int gen, const char *name, int count,
                             void *arg),
                  void *arg);

/* The spec's name for a generator, or "unknown". Never NULL. */
const char *sf2_gen_name(int gen);

/* The spec's default, for a generator not present in the file. */
short sf2_gen_default(int gen);

#endif /* SF2GEN_H */
