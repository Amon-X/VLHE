/*
 * autovoice.c - automatic voice reduction for vmidid, on TiMidity's
 * model. See autovoice.h for the model and why; the line references
 * below are TiMidity++ 2.11.3's playmidi.c.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 */

#include <stdlib.h>
#include <string.h>

#include "autovoice.h"

/* Each setting's range, in struct order. */
static const int lo_lim[AUTOVOICE_NSET] = {  5,  0,  10,   20,  1, 0 };
static const int hi_lim[AUTOVOICE_NSET] = { 95, 90, 100, 5000, 64, 1 };
static const char *const names[AUTOVOICE_NSET] = {
    "drain", "emergency", "healthy", "settle", "floor", "tails"
};

void
autovoice_get(const autovoice *a, int v[AUTOVOICE_NSET])
{
    v[0] = a->drain;     v[1] = a->emergency; v[2] = a->healthy;
    v[3] = a->settle_ms; v[4] = a->floor;     v[5] = a->tails;
}

const char *
autovoice_set(autovoice *a, const int v[AUTOVOICE_NSET])
{
    int i;

    for (i = 0; i < AUTOVOICE_NSET; i++)
        if (v[i] < lo_lim[i] || v[i] > hi_lim[i])
            return "a value is out of its range";
    /* THE THREE FILLS MUST BE IN ORDER, or "draining" and "healthy"
     * overlap and it restores what it just cut. Refused, not clamped. */
    if (!(v[1] < v[0]))
        return "emergency must be below drain";
    if (!(v[0] < v[2]))
        return "drain must be below healthy";

    a->drain = v[0];     a->emergency = v[1]; a->healthy = v[2];
    a->settle_ms = v[3]; a->floor = v[4];     a->tails = v[5];
    return NULL;
}

const char *
autovoice_parse(autovoice *a, const char *spec)
{
    int  v[AUTOVOICE_NSET];
    char name[16];
    const char *p = spec;

    autovoice_get(a, v);
    while (p != NULL && *p != '\0') {
        const char *eq = strchr(p, '=');
        const char *end = strchr(p, ',');
        char *num_end;
        long n;
        int  i, len;

        if (eq == NULL || (end != NULL && end < eq))
            return "expected name=value";
        len = (int) (eq - p);
        if (len <= 0 || len >= (int) sizeof name)
            return "unknown setting";
        memcpy(name, p, (size_t) len);
        name[len] = '\0';
        for (i = 0; i < AUTOVOICE_NSET; i++)
            if (strcmp(name, names[i]) == 0)
                break;
        if (i == AUTOVOICE_NSET)
            return "unknown setting";
        n = strtol(eq + 1, &num_end, 10);
        if (num_end == eq + 1 || (*num_end != ',' && *num_end != '\0'))
            return "not a number";
        v[i] = (n < -100000L || n > 100000L) ? -1 : (int) n;
        p = (*num_end == ',') ? num_end + 1 : NULL;
    }
    return autovoice_set(a, v);
}

void
autovoice_restart(autovoice *a, int on, int ceiling)
{
    if (ceiling < 1)
        ceiling = 1;
    a->on        = on ? 1 : 0;
    a->ceiling   = ceiling;
    a->cur       = ceiling;
    /* START AS IF FULL - a fresh channel has no history of draining,
     * and starting at 0 would read as an emergency on the first block. */
    a->avg       = 1000;
    a->win_ref   = 1000;
    a->win_us    = 0;
    a->trend     = 0;
    a->healthy_us = 0;
    a->hold_us   = 0;
    a->settle_us = 0;
    /* ok_nv STARTS AT HALF THE CEILING, as TiMidity's 32 is half its
     * default 64 voices (:97) - so the first draining has somewhere to
     * go. Starting at the ceiling made the first draining do nothing,
     * and nothing after it, since nothing lowered ok_nv. */
    a->ok_total  = ceiling / 2 > 0 ? ceiling / 2 : 1;
    a->ok_counts = 1;
    a->ok_age_us = 0;
    a->max_good  = 1;
    a->min_bad   = ceiling;
    a->cuts      = 0;
    a->restores  = 0;
    a->steps     = 0;
    a->climbing  = 0;
    a->shed      = 0;
    a->lowest    = ceiling;
}

void
autovoice_init(autovoice *a, int on, int ceiling)
{
    static const int def[AUTOVOICE_NSET] = {
        AUTOVOICE_DRAIN, AUTOVOICE_EMERGENCY, AUTOVOICE_HEALTHY,
        AUTOVOICE_SETTLE, AUTOVOICE_FLOOR, AUTOVOICE_TAILS
    };

    (void) autovoice_set(a, def);
    autovoice_restart(a, on, ceiling);
}

int
autovoice_manual(autovoice *a)
{
    int was = a->on;

    a->on = 0;
    return was;
}

/*
 * WHY IT JUDGES OVER WINDOWS - fixed 2026-10-03, after the first target
 * run of this model jumped back and forth: 616 cuts and 366 restores in
 * one session, 55 of the cuts with the average fill at or ABOVE 50%
 * ("voices -> 8 (automatic: buffer 87% and falling, 0 tails shed)" -
 * tests/logs/2026-10-03-86box-timidity-autovoice-capture-seed). Three
 * causes, all of them judging on too little:
 *
 *   - THE EMERGENCY took ONE block under `emergency'. vsound empties
 *     the 4-block ring in ~10 ms ticks, so a reading taken just after a
 *     tick can be near empty by chance. Now it also needs the AVERAGE
 *     below `drain' - sustained, not a single sample.
 *   - "FALLING" compared this block's average with the last block's. A
 *     jittery but steady buffer satisfies that about half the time. Now
 *     the trend is measured over each `settle_ms' window and counts only
 *     past 2 points (20 thousandths); a FLAT buffer is draining only
 *     when it sits below half of `drain'.
 *   - RECOVERY WAS IMMEDIATE. Now the average must stay healthy for four
 *     settle windows before the full ceiling comes back, and after a
 *     restore no routine cut follows for four windows either - an
 *     emergency still acts.
 */
int
autovoice_block(autovoice *a, long fill, long block_us, int active,
                int held, int *shed_out)
{
    long drain = a->drain * 10L, healthy = a->healthy * 10L;
    int  falling, rising, emergency, ret = 0;

    if (shed_out != NULL)
        *shed_out = 0;
    if (!a->on || block_us <= 0)
        return 0;
    if (fill < 0)
        fill = 0;
    if (fill > 1000)
        fill = 1000;
    if (held > active)
        held = active;

    a->avg += (fill - a->avg) / 8;

    /* THE TREND, PER WINDOW - see above. */
    a->win_us += block_us;
    if (a->win_us >= a->settle_ms * 1000L) {
        a->trend = a->avg < a->win_ref - 20 ? -1
                 : a->avg > a->win_ref + 20 ? 1 : 0;
        a->win_ref = a->avg;
        a->win_us = 0;
    }
    falling = a->trend < 0
              || (a->trend == 0 && a->avg < drain / 2);  /* sitting low */
    rising  = a->trend > 0;
    /* AN EMERGENCY IS AN EMPTY BLOCK WITH THE AVERAGE LOW TOO. */
    emergency = fill < a->emergency * 10L && a->avg < drain;

    if (a->settle_us > 0)
        a->settle_us -= block_us;
    if (a->hold_us > 0)
        a->hold_us -= block_us;
    if (a->avg >= healthy && a->trend >= 0)
        a->healthy_us += block_us;
    else
        a->healthy_us = 0;

    /*
     * --- LEARN (:4051-4078, :4095-4100, :4172-4176) ----------------
     *
     * ok_nv: TiMidity averages in the voice count only when it is
     * ABOVE min_bad with the buffer holding, or one more than the
     * count when the ceiling binds and the buffer rises - never every
     * block. Every block is what a quiet passage would drag down, and
     * a quiet passage is exactly where the first version went wrong.
     * Its samples are halved each second so old passages fade.
     * max_good: the most voices seen with the buffer healthy. min_bad:
     * the fewest seen while it drained, short of an emergency.
     */
    if (a->avg >= drain && active > a->min_bad) {
        a->ok_total += active;
        a->ok_counts++;
    } else if (a->avg >= drain && rising && active >= a->cur) {
        a->ok_total += active + 1;      /* :4060 - "increase polyphony
                                         * when it is too low" */
        a->ok_counts++;
    }
    a->ok_age_us += block_us;
    if (a->ok_age_us >= 1000000L && a->ok_counts > 1) {
        a->ok_total >>= 1;
        a->ok_counts >>= 1;
        a->ok_age_us = 0;
    }
    if (a->avg >= healthy && active > a->max_good)
        a->max_good = active;
    if (a->avg < drain && falling && !emergency && active > 0
        && active < a->min_bad)
        a->min_bad = active;
    /* "REDUCE POLYPHONY WHEN LOOSING BUFFER" (:4066-4070): while it
     * drains, min_bad - the fewest voices seen draining - is averaged
     * into ok_nv, so the learned count comes DOWN towards what the
     * machine was carrying when it started to lose. Left out of the
     * first port (design/54 D71, 2026-10-08): the emergency's drop to
     * the held notes hid the need, and without it a ceiling that has
     * reached ok can never go lower. Only once min_bad has been seen -
     * averaging in the ceiling would raise ok, not lower it. */
    else if (a->avg < drain && falling && a->min_bad < a->ceiling) {
        a->ok_total += a->min_bad;
        a->ok_counts++;
    }

    /* --- DRAINING: shed tails, lower the ceiling (:4087-4158) ----- */
    if ((a->avg < drain && falling) || emergency) {
        int tails = active - held;
        int shed = 0, left, ok, target;

        if (a->settle_us > 0)
            return 0;
        /* JUST RESTORED: give it the hold before a routine cut. */
        if (a->hold_us > 0 && !emergency)
            return 0;

        /* TAILS FIRST. Routinely, not below min_bad (:4137); in an
         * emergency, all of them (:4117-4132, short of ON notes). */
        if (a->tails && tails > 0) {
            if (emergency)
                shed = tails;
            else {
                shed = active - a->min_bad;
                if (shed > tails)
                    shed = tails;
                if (shed < 0)
                    shed = 0;
            }
        }
        left = active - shed;

        ok = (int) (a->ok_total / (a->ok_counts > 0 ? a->ok_counts : 1));
        target = a->cur;
        if (a->cur > left && left > ok)
            target = left;              /* down to what is left */
        else if (a->cur > ok)
            target = ok;                /* down to the learned ok */
        /* AND NOT BELOW THE LEARNED ok IN AN EMERGENCY EITHER - design/54
         * D71, 2026-10-08. This used to force `target = left' when the
         * ring was empty, so on the Soyo a 512-voice ceiling fell to the
         * 8 notes HELD and stayed there until a restore, stealing the
         * next phrase. TiMidity's drastic path (:4146-4157) lowers the
         * ceiling to what is left only if that is ABOVE ok_nv, and to
         * ok_nv otherwise - the kill is drastic, the ceiling is not. The
         * two branches above already say exactly that. The tails are
         * still all shed, which is the relief. */

        /* CONSERVATIVE (:3347): never below the notes being held, so a
         * held note is not stolen by the very next one. Then the floor. */
        if (target < held)
            target = held;
        if (target < a->floor)
            target = a->floor;

        if (target < a->cur) {
            a->cur = target;
            a->cuts++;
            if (target < a->lowest)
                a->lowest = target;
            /* A CUT LOWERS max_good WHEN IT WAS SET TOO HIGH (16a item
             * 3): the next restore aims where the song last held, not
             * at a peak once seen while the buffer was still full. */
            if (a->max_good > target)
                a->max_good = target;
            a->climbing = 0;
            ret = target;
        }
        if (shed > 0 || ret) {
            a->shed += shed;
            a->settle_us = a->settle_ms * 1000L;
        }
        if (shed_out != NULL)
            *shed_out = shed;
        return ret;
    }

    /* --- HEALTHY: back to what held, then creep (design/21 16a) ----- */
    /*
     * TiMidity RESTORES ALL AT ONCE (:4167-4188, restore_voices(0)),
     * and so did this until 2026-10-08. On the Red Hat 6 Pentium Pro
     * at 22050 - a machine that carries 16-20 of 64 - every "healthy
     * again" went straight to 64, the next dense passage overloaded it
     * at once, and it cut again: 69 cuts and 33 restores in 178 s,
     * the buffer at 0% several times (tests/logs/2026-10-06-86box-
     * redhat60-gmstriving-22050). TiMidity gets away with the jump
     * partly because it has a cheaper lever first - it drops to
     * linear interpolation before it touches voices.
     *
     * NOW: after four healthy windows, back to max_good - the most
     * voices seen with the buffer healthy, which a cut has lowered to
     * where it cut - and from there a STEP towards the ceiling (a
     * quarter of the gap, at least 4) every further four healthy
     * windows, ONLY WHILE THE LIMIT BINDS: with fewer voices sounding
     * than the ceiling the song is not asking for more, and a step up
     * would be tested at the next dense passage, the moment it costs
     * most. A machine with headroom is back at 64 in a couple of
     * seconds; one without stops rising where it starts to drain.
     */
    if (a->cur < a->ceiling
        && a->healthy_us >= 4L * a->settle_ms * 1000L) {
        int target = a->max_good > a->cur ? a->max_good : a->cur;

        if (target > a->ceiling)
            target = a->ceiling;
        if (target == a->cur) {
            int gap = a->ceiling - a->cur, step = gap / 4;

            if (active < a->cur) {
                a->healthy_us = 0;      /* not asked for; look again */
                return 0;
            }
            if (step < 4)
                step = 4;
            target = a->cur + step;
            if (target > a->ceiling)
                target = a->ceiling;
        }
        if (!a->climbing) {
            a->restores++;
            a->climbing = 1;
        }
        a->steps++;
        a->cur = target;
        a->healthy_us = 0;
        a->hold_us = 4L * a->settle_ms * 1000L;
        /* RESET ok_nv TO max_good, as TiMidity does when out of
         * danger - the next draining starts from what was known good. */
        a->ok_total = (long) a->max_good * a->ok_counts;
        if (target >= a->ceiling)
            a->climbing = 0;
        return target;
    }
    return 0;
}
