/*
 * modtab.h - the four SF2.01 modulator source curves, 0..1 in 16.16.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * GENERATED, not hand-written: tests/mkmodtab.py rebuilds it.
 *
 * SF2.01 section 8.2.4 defines four source types. Only the concave
 * one has a formula in the spec:
 *
 *     output = -20/96 * log10((value^2)/(range^2))
 *
 * and convex is defined only in prose - "the same curve as the
 * concave curve, except the start and end points are reversed" -
 * which is a point reflection through (0.5, 0.5), not a horizontal
 * flip. Getting that wrong gives a curve that is subtly too bright
 * at low input and passes a casual eyeball.
 *
 * 128 ENTRIES, INDEXED DIRECTLY BY A 7-BIT MIDI VALUE. Every
 * modulator source in every soundfont surveyed for
 * design/18-modulator-layer.md is 7-bit; the sole 14-bit source
 * (pitch wheel) is scaled by the caller.
 *
 * A modulator's contribution is therefore
 *
 *     (amount * curve[input]) >> MODTAB_SHIFT
 *
 * in the destination's own units - centibels for attenuation, cents
 * for filter cutoff - which is what the SF2 model sums.
 *
 * THE `D' BIT INVERTS THE OUTPUT, NOT THE INPUT. Get this wrong and
 * the answer is out by a factor of 25, not by a rounding error - so
 * it is written here rather than left to be re-derived.
 *
 *     NEGATIVE (D=1):   contribution = amount * (ONE - curve[v])
 *     POSITIVE (D=0):   contribution = amount *        curve[v]
 *
 * NOT `curve[127 - v]', which is the natural first guess and which
 * this project wrote THREE TIMES - twice in throwaway analysis and
 * once in C - before checking it against a number.
 *
 * THE CHECK, and any change here must still pass it: SF2.01 section
 * 8.4.1 is velocity -> initialAttenuation, amount 960 cB, concave,
 * unipolar, NEGATIVE. The spec test suite states the answer outright
 * - `vmidi/tests/sfspec/README.md', test 13A: "the difference
 * between velocity 127 and 111 should be 2.34 dB".
 *
 *     att(v) = (960 * (MODTAB_ONE - modtab_concave[v])) >> 16
 *
 *     vel 127 ->   0 cB      vel 111 ->  23 cB      = 2.30 dB
 *
 * 2.30 against the stated 2.34 is integer centibel truncation, which
 * is the unit the renderer works in anyway. `curve[127 - v]' gives
 * 60.00 dB, which is how the error was caught.
 *
 * Three independent routes agree on 2.34: the spec's own formula,
 * the suite's stated value, and `render.c's existing hardcoded
 * `gain = (gain * vel * vel) / (127 * 127)', which is the same curve
 * expressed in the gain domain. FLUIDSYNTH MEASURES 3.53 dB HERE AND
 * IS THE OUTLIER - see `vmidi/tests/sfspec/SCORE-12-15.md'. Do not
 * "correct" this toward the reference.
 */

#ifndef MODTAB_H
#define MODTAB_H

#define MODTAB_N     128
#define MODTAB_SHIFT 16
#define MODTAB_ONE   (1L << MODTAB_SHIFT)

/* type 0: linear */
static const long modtab_linear[MODTAB_N] = {
         0,    516,   1032,   1548,   2064,   2580,   3096,   3612,
      4128,   4644,   5160,   5676,   6192,   6708,   7224,   7740,
      8257,   8773,   9289,   9805,  10321,  10837,  11353,  11869,
     12385,  12901,  13417,  13933,  14449,  14965,  15481,  15997,
     16513,  17029,  17545,  18061,  18577,  19093,  19609,  20125,
     20641,  21157,  21673,  22189,  22705,  23221,  23737,  24253,
     24770,  25286,  25802,  26318,  26834,  27350,  27866,  28382,
     28898,  29414,  29930,  30446,  30962,  31478,  31994,  32510,
     33026,  33542,  34058,  34574,  35090,  35606,  36122,  36638,
     37154,  37670,  38186,  38702,  39218,  39734,  40250,  40766,
     41283,  41799,  42315,  42831,  43347,  43863,  44379,  44895,
     45411,  45927,  46443,  46959,  47475,  47991,  48507,  49023,
     49539,  50055,  50571,  51087,  51603,  52119,  52635,  53151,
     53667,  54183,  54699,  55215,  55731,  56247,  56763,  57279,
     57796,  58312,  58828,  59344,  59860,  60376,  60892,  61408,
     61924,  62440,  62956,  63472,  63988,  64504,  65020,  65536
};

/* type 1: concave - the spec's formula */
static const long modtab_concave[MODTAB_N] = {
         0,   8088,  16308,  21117,  24528,  27175,  29337,  31165,
     32749,  34145,  35395,  36525,  37557,  38506,  39385,  40203,
     40969,  41688,  42365,  43007,  43615,  44194,  44745,  45272,
     45777,  46261,  46726,  47174,  47605,  48021,  48423,  48812,
     49189,  49554,  49908,  50251,  50586,  50910,  51227,  51535,
     51835,  52128,  52414,  52693,  52965,  53232,  53493,  53748,
     53997,  54242,  54481,  54716,  54946,  55172,  55394,  55612,
     55825,  56035,  56241,  56444,  56644,  56840,  57032,  57222,
     57409,  57593,  57774,  57952,  58128,  58301,  58472,  58640,
     58806,  58969,  59131,  59290,  59447,  59602,  59755,  59906,
     60055,  60202,  60348,  60492,  60634,  60774,  60913,  61050,
     61185,  61319,  61452,  61583,  61713,  61841,  61968,  62093,
     62217,  62340,  62462,  62582,  62701,  62819,  62936,  63052,
     63167,  63280,  63392,  63504,  63614,  63723,  63832,  63939,
     64045,  64151,  64255,  64359,  64462,  64563,  64664,  64764,
     64864,  64962,  65060,  65156,  65253,  65348,  65442,  65536
};

/* type 2: convex - concave with the endpoints reversed */
static const long modtab_convex[MODTAB_N] = {
         0,     94,    188,    283,    380,    476,    574,    672,
       772,    872,    973,   1074,   1177,   1281,   1385,   1491,
      1597,   1704,   1813,   1922,   2032,   2144,   2256,   2369,
      2484,   2600,   2717,   2835,   2954,   3074,   3196,   3319,
      3443,   3568,   3695,   3823,   3953,   4084,   4217,   4351,
      4486,   4623,   4762,   4902,   5044,   5188,   5334,   5481,
      5630,   5781,   5934,   6089,   6246,   6405,   6567,   6730,
      6896,   7064,   7235,   7408,   7584,   7762,   7943,   8127,
      8314,   8504,   8696,   8892,   9092,   9295,   9501,   9711,
      9924,  10142,  10364,  10590,  10820,  11055,  11294,  11539,
     11788,  12043,  12304,  12571,  12843,  13122,  13408,  13701,
     14001,  14309,  14626,  14950,  15285,  15628,  15982,  16347,
     16724,  17113,  17515,  17931,  18362,  18810,  19275,  19759,
     20264,  20791,  21342,  21921,  22529,  23171,  23848,  24567,
     25333,  26151,  27030,  27979,  29011,  30141,  31391,  32787,
     34371,  36199,  38361,  41008,  44419,  49228,  57448,  65536
};

/*
 * type 3: SWITCH. No table - it is a step at the midpoint, and the
 * spec's own wording is "the value is 1 if the controller is at or
 * above 64, 0 otherwise" for the positive form. Kept as a macro so
 * the four types read alike at the call site.
 */
#define modtab_switch(v)  ((v) >= (MODTAB_N / 2) ? MODTAB_ONE : 0L)

#endif /* MODTAB_H */
