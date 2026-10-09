/*
 * sf2mod.h - SF2.01 modulators: the ten defaults and the 9.5.1 merge.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * sf2gen.c resolves a zone's GENERATORS - instrument absolute, preset
 * additive. This resolves its MODULATORS, which follow a different and
 * more particular set of rules (SF2.01 section 9.5.1, the bullets
 * after "Destination = Generator Value + Mod() + Mod()"):
 *
 *   - a modulator is IDENTICAL to another if its source, destination,
 *     amount source and transform match. The amount may differ; that
 *     is the whole point of identity.
 *   - the ten defaults (8.4.1-8.4.10) are implicit at the INSTRUMENT
 *     level.
 *   - a global instrument zone modulator identical to a default
 *     SUPERSEDES it; a local instrument zone one identical to a default
 *     or a global supersedes that. Non-identical ones ADD.
 *   - a local preset zone modulator identical to a global preset zone
 *     one supersedes it - within the preset level only.
 *   - a preset modulator then ALWAYS ADDS to the instrument level,
 *     identical or not; identical means the amounts sum into one.
 *
 * The output is a flat list: one entry per distinct (source, dest,
 * amount source, transform), amounts already merged, ready to be
 * evaluated at note-on. design/18-modulator-layer.md sections 5, 9
 * and 10.
 *
 * THE ONE RULE THAT IS NOT IN THE SPEC - "NORMALISE", design/18 10d.
 * Default 8.4.2 (velocity -> initialFilterFc) is implemented in the
 * form fluidsynth registers - secondary source switch POSITIVE (D=0),
 * a ramp above velocity 64 and nothing below - because that is the
 * shape of the AWE32 hardware and the spec's own D=1 form jumps at 63.
 * But every font that mentions 8.4.2 does so in the SPEC's form:
 * 8764 instrument-level cancels and ten preset-level negations across
 * 13 fonts, and not one use of the flipped form. So any modulator of
 * 8.4.2's SHAPE - velocity linear unipolar negative to initialFilterFc
 * with a velocity switch as amount source - is treated in the
 * synth's own form regardless of its D bit, for identity and for
 * evaluation. In 13 fonts that shape is only ever a statement about
 * the default; normalising it is what makes those statements come
 * out as their authors meant.
 *
 * TWO IDENTICAL MODULATORS IN ONE ZONE SUM. The spec's supersede
 * rules are all BETWEEN places - local over global, instrument over
 * default - and its identity rule says an identical pair "is
 * equivalent to a single modulator whose amount is the sum". So a
 * later identical modulator from an EARLIER place replaces; from the
 * SAME zone it adds. Normalise makes this matter: under the 2.04
 * mode a font's +2400 negation folds onto the ramp form and becomes
 * identical to its -5000 partner in the same preset zone. Summed they
 * are the -2600 the author wrote; superseded, one of them vanishes.
 *
 * FOUR WAYS TO PLAY THE VELOCITY-FILTER DEFAULT, one table row each,
 * chosen by the low bits of the merge flags (design/18 10f, the
 * user's decision 2026-09-08):
 *
 *   SF2_MOD_VF_AWE   the default. The 2.01 shape with the switch
 *                    flipped: a ramp from velocity 127 to 64 and
 *                    nothing below, the AWE32's own shape.
 *   SF2_MOD_VF_201   SF2.01 8.4.2 exactly as printed: nothing from
 *                    127 to 64, then a jump to -1200 at 63. The spec's
 *                    successor calls it a mistake; a user may want it.
 *   SF2_MOD_VF_204   SF2.04's default: a plain -2400 ramp, no switch.
 *   SF2_MOD_VF_NONE  no default at all. What fluidsynth, TiMidity,
 *                    awesfx and BASSMIDI do.
 *
 * NORMALISE follows the row: a modulator of 8.4.2's shape is taken in
 * the ACTIVE row's form - so a font's 2.01-form cancel (8764 of them)
 * switches off whichever default is playing, and the ten preset-level
 * negations cancel it exactly, under AWE, 2.01 and 2.04 alike. Under
 * NONE those statements are about a default that does not exist and
 * are no-ops. A 2.04-shaped modulator (no amount source) is NOT
 * folded onto a switch row: with a real amount it is a real
 * modulator (the spec test's 14B is one), and only its amount could
 * tell a cancel from an effect - the misreading design/18 section 1
 * records. So under AWE and 2.01 a 2.04-form cancel leaves the
 * default playing, which is what a 2.01 synth does and what the
 * suite's own 14D/14E pair is designed to reveal.
 *
 * C89, gcc 2.95.2 clean. No floating point: the merge is integer
 * bookkeeping; evaluation (render.c) uses the 16.16 tables in
 * synth/modtab.h.
 */
#ifndef SF2MOD_H
#define SF2MOD_H

#include "sf2.h"

/*
 * ROOM FOR THE WORST CASE SEEN, WITH MARGIN. Across the 13 fonts on
 * this disk the largest zone carries 7 instrument and 9 preset
 * modulators (modid.py, 2026-09-07), so a note can meet at most
 * 10 defaults + 7 + 7 + 9 + 9 = 42 before any merge. 48 leaves a
 * little over that; a font that exceeds it has its surplus dropped
 * and counted, never overrun.
 */
#define SF2_MAX_MODS        48

/* The spec's implicit instrument-level modulators, 8.4.1 - 8.4.10. */
#define SF2_MOD_NDEFAULTS   10

/* Where an entry came from - for a listing, not for evaluation. */
#define SF2_MOD_FROM_DEFAULT  0
#define SF2_MOD_FROM_INST     1
#define SF2_MOD_FROM_PRESET   2

/* Flags to sf2_mod_merge(): the velocity-filter default, low two bits. */
#define SF2_MOD_VF_MASK       0x0003
#define SF2_MOD_VF_AWE        0x0000    /* the default: 2.01, switch flipped */
#define SF2_MOD_VF_NONE       0x0001    /* no default - fluidsynth's way    */
#define SF2_MOD_VF_201        0x0002    /* SF2.01 8.4.2 as printed          */
#define SF2_MOD_VF_204        0x0003    /* SF2.04: a plain -2400 ramp       */

/*
 * THE VOLUME LAW, bit 2: what velocity, CC 7 and CC 11 do to level.
 *
 *   SF2_MOD_LAW_SPEC    the default. 8.4.1, 8.4.5 and 8.4.7 as printed:
 *                       concave, 960 cB, so gain = (v/127)^2 for each -
 *                       40 log10(127/v) dB. fluidsynth and the AWE32
 *                       driver's default law (AWE_MD_NEW_VOLUME_CALC=1)
 *                       agree with it within 0.6 dB.
 *   SF2_MOD_LAW_LINEAR  the same three rows at 480 cB, so gain = v/127
 *                       for each - 20 log10(127/v) dB. The AWE32
 *                       driver's OLD law (awe_wave.c:2067-2074:
 *                       vel * vol * expr / 127^2 through vol_table[])
 *                       to within 0.45 dB at every value, and near what
 *                       2.2's OPL3 driver plays. Six dB gentler at
 *                       velocity 64, twelve on a soft note at a low
 *                       fader - design/22 section 16, the user's
 *                       decision 2026-09-08 after Doom's music sat 20 dB
 *                       under its effects.
 *
 * Only the DEFAULT rows change: a font's own modulator onto
 * initialAttenuation keeps its amount, and one identical to a default
 * supersedes or adds exactly as before, onto 480 instead of 960.
 */
#define SF2_MOD_LAW_MASK      0x0004
#define SF2_MOD_LAW_SPEC      0x0000
#define SF2_MOD_LAW_LINEAR    0x0004

/*
 * The user's spelling of a mode, for a -F option: "awe", "2.01",
 * "2.04", "none" - or the number, 1 2 3 0 in that order, so a config
 * file can carry either. Returns the flags, or -1 for anything else.
 */
int sf2_mod_vf_parse(const char *s);

/* The name for a banner or a log line. */
const char *sf2_mod_vf_name(unsigned flags);

/* The law, for a -L option: "spec" or "linear" - or 0, 1. */
int sf2_mod_law_parse(const char *s);
const char *sf2_mod_law_name(unsigned flags);

/* The same law described for a READER rather than for matching a
 * flag - "linear (gentler, Sound Blaster era)". The bare names put
 * -F's value and -L's value side by side in a banner and AWE names
 * three different things between them; see the function's comment. */
const char *sf2_mod_law_desc(unsigned flags);

/* The 8.4.2 row this mode plays, or NULL under SF2_MOD_VF_NONE. */
const sf2_mod *sf2_mod_velfilter_row(unsigned flags);

/* Which of the five places an entry was last touched from. */
#define SF2_MOD_ZONE_DEFAULT  0
#define SF2_MOD_ZONE_IGLOBAL  1
#define SF2_MOD_ZONE_ILOCAL   2
#define SF2_MOD_ZONE_PGLOBAL  3
#define SF2_MOD_ZONE_PLOCAL   4

typedef struct {
    sf2_mod         mod[SF2_MAX_MODS];
    unsigned char   from[SF2_MAX_MODS];   /* SF2_MOD_FROM_*, the level */
    unsigned char   zone[SF2_MAX_MODS];   /* SF2_MOD_ZONE_*, the zone  */
    int             n;
    int             dropped;              /* would not fit, or invalid */
} sf2_modlist;

/* The ten defaults with 8.4.2 in the AWE form; the merge substitutes
 * the active mode's row for it. */
const sf2_mod *sf2_mod_defaults(void);

/* SF2.01 9.5.1: source, destination, amount source, transform. */
int sf2_mod_identical(const sf2_mod *a, const sf2_mod *b);

/* Is this a modulator of 8.4.2's shape? (see NORMALISE above) */
int sf2_mod_is_velfilter(const sf2_mod *m);

/*
 * Section 8.2.1's palette rules: a modulator naming a reserved or
 * illegal source, an unsupported transform or an out-of-range
 * destination is IGNORED WHOLE - the spec's words - rather than
 * partly applied. Returns 1 if the modulator may be used.
 */
int sf2_mod_valid(const sf2_mod *m);

/*
 * THE MERGE. Instrument modulators from `imods' in the two ranges
 * [ig0,ig1) (global zone) and [il0,il1) (local zone); preset ones from
 * `pmods' in [pg0,pg1) and [pl0,pl1). A zone with no global has an
 * empty range. Fills `ml' from scratch.
 */
void sf2_mod_merge(sf2_modlist *ml,
                   const sf2_mod *imods, int ig0, int ig1, int il0, int il1,
                   const sf2_mod *pmods, int pg0, int pg1, int pl0, int pl1,
                   unsigned flags);

#endif /* SF2MOD_H */
