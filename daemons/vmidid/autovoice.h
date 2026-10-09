/*
 * autovoice.h - automatic voice reduction for vmidid, on TiMidity's
 * model.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/21 section 16. REBUILT 2026-10-02 after the first version
 * failed on 86Box (tests/logs/2026-10-02-86box-autovoice-gmstriving/):
 * it acted on how BUSY the render was, cut to 8 voices in a quiet
 * opening while the synth was keeping up, and stole two thirds of
 * gmstriving's notes. The user: "yes to build it based on timiditys
 * model thats where we got the idea for it."
 *
 * TiMIDITY'S MODEL (playmidi.c:4012-4194, read in full in design/21):
 *
 *   - ACT ONLY WHILE THE OUTPUT BUFFER DRAINS. Ours is vmidid's own
 *     vsound channel, read with SNDCTL_DSP_GETOSPACE before each write
 *     - that one client's ring, not the shared /dev/dsp. A synth that
 *     keeps up never loses fill and is never touched.
 *   - SHED RELEASE TAILS FIRST, quietest first, drums protected
 *     (render_shed_tails()) - optional here, `tails'.
 *   - LOWER THE CEILING CONSERVATIVELY: to what is left, or to the
 *     learned "ok" count, and never below the notes being HELD.
 *   - LEARN: ok_nv, a decaying average of the voice count while the
 *     buffer holds; max_good, the most voices seen while healthy;
 *     min_bad, the fewest seen while draining. A routine shed never
 *     goes below min_bad.
 *   - RESTORE TO WHAT HELD, THEN CREEP (design/21 16a, 2026-10-08 -
 *     TiMidity restores all at once, and on a machine whose limit is
 *     mid-range that overloaded it again every time): when the buffer
 *     has been healthy, back to max_good, then a step towards the
 *     ceiling every further healthy spell, and only while the limit
 *     binds (active voices at the ceiling). A cut lowers max_good to
 *     where it cut, so the next restore aims where the song last held.
 *
 * PURE ARITHMETIC: no clock, no I/O. vmidid reads the fill and the
 * voice counts and hands them in; the host test drives it with made-up
 * ones. Every number is a setting - `-a', `set autovoice', and
 * [Midi Settings] AutoVoice* through Apply.
 */

#ifndef AUTOVOICE_H
#define AUTOVOICE_H

/* THE SETTINGS, their defaults and ranges. Fills are per cent of the
 * channel's ring. The ring is small (4 x 1024 bytes, ~23 ms) and vsound
 * empties it in ~10 ms ticks, so a synth that keeps up reads anywhere
 * from about half to full - hence 50, not TiMidity's 75, for draining. */
#define AUTOVOICE_NSET        6
#define AUTOVOICE_DRAIN      50     /* reduce when the average is below
                                     * this AND falling (5..95)          */
#define AUTOVOICE_EMERGENCY  10     /* this block below it: drastic
                                     * (0..90)                           */
#define AUTOVOICE_HEALTHY    75     /* restore at or above, not falling
                                     * (10..100)                         */
#define AUTOVOICE_SETTLE    100     /* ms after acting (20..5000)        */
#define AUTOVOICE_FLOOR      16     /* never fewer voices (1..64). 16
                                     * since 2026-10-08 (was 8): the P1
                                     * holds 16 at 22050 with effects on
                                     * and a Pentium Pro 133 cuts to
                                     * 16-20 there - tests/logs
                                     * 2026-09-10-p1-59, 2026-10-06-
                                     * 86box-redhat60-gmstriving-22050 */
#define AUTOVOICE_TAILS       1     /* shed release tails (0 or 1)       */

typedef struct {
    /* --- the settings, in this order --------------------------- */
    int  drain, emergency, healthy, settle_ms, floor, tails;

    /* --- the state ---------------------------------------------- */
    int  on;
    int  ceiling;       /* -p: what a restore returns to               */
    int  cur;           /* the ceiling in force                        */
    long avg;           /* fill, averaged over ~8 blocks, thousandths  */
    long win_ref;       /* the average at the start of this window     */
    long win_us;        /* how far into the window - settle_ms long    */
    int  trend;         /* -1 falling, 0 flat, +1 rising, per window   */
    long healthy_us;    /* how long the average has stayed healthy     */
    long hold_us;       /* after a restore: no routine cut for this    */
    long settle_us;     /* time left before acting again               */
    long ok_total;      /* ok_nv = ok_total / ok_counts                */
    long ok_counts;
    long ok_age_us;     /* halve the ok_nv samples every second        */
    int  max_good;      /* most voices seen while healthy              */
    int  min_bad;       /* fewest seen while draining                  */
    long cuts;          /* times it lowered the ceiling, for the log   */
    long restores;      /* times it started putting it back            */
    long steps;         /* the steps those restores took, in all       */
    int  climbing;      /* between a restore's first step and the top  */
    long shed;          /* tails it asked to shed                      */
    int  lowest;        /* the lowest ceiling, for the log             */
} autovoice;

/* State reset, settings to the DEFAULTS. */
void autovoice_init(autovoice *a, int on, int ceiling);

/* State reset, settings KEPT - a new ceiling or the on/off switch. */
void autovoice_restart(autovoice *a, int on, int ceiling);

/*
 * THE SIX SETTINGS AT ONCE, in struct order. All or nothing: each in
 * its range AND emergency < drain < healthy, or nothing changes and
 * the reason comes back. NULL means accepted.
 */
const char *autovoice_set(autovoice *a, const int v[AUTOVOICE_NSET]);

/* "drain=50,settle=100,..." over the CURRENT settings; NULL or why. */
const char *autovoice_parse(autovoice *a, const char *spec);

void autovoice_get(const autovoice *a, int v[AUTOVOICE_NSET]);

/*
 * ONE BLOCK. `fill' is the channel ring's fill in thousandths, read
 * before this block's write; `block_us' the block's length; `active'
 * the voices sounding and `held' how many of those are not in their
 * release. Returns the new ceiling if it should change, else 0, and
 * sets *shed_out to how many release tails the caller should shed
 * (render_shed_tails()). Between blocks, where both are safe.
 */
int autovoice_block(autovoice *a, long fill, long block_us, int active,
                    int held, int *shed_out);

/* A SIGUSR nudge: the hand wins, it stops for the run. 1 if it was on. */
int autovoice_manual(autovoice *a);

#endif
