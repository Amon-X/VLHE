/*
 * vlhe_rates.h - the synth's sample rates, one list for every page.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * ONE LIST, 2026-10-04 - the user: Midi Settings offered 44100/22050/
 * 11025 and the Render page 22050/32000/44100/48000, so a rate set for
 * playing could not be chosen for rendering. Both pages, and the
 * backend's check on what Midi Settings stores, now read this.
 *
 * WHY THESE FOUR. Every card this project meets takes all four - the
 * SB16 (5000-44100, in Hz), the ESS Solo-1 (32000 is 768000/24
 * exactly), the es1371 (through its SRC) and the emu10k1 (resampled
 * in hardware); an SB Pro clamps stereo to 22050 whichever is asked.
 * 32000 is MPEG audio's own rate, so LAME encodes it unresampled.
 *
 * WHY NOT 48000. The synth refuses anything above RENDER_RATE_MAX,
 * 46340 (daemons/vmidid/render.h) - the pitch step's 32-bit bound -
 * so the Render page's 48000 failed every time it was picked.
 *
 * Highest first: the first entry is the default and the fallback for
 * a rate this list does not hold.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_RATES_H
#define VLHE_RATES_H

#define VLHE_SYNTH_RATES   { 44100, 32000, 22050, 11025 }
#define VLHE_SYNTH_NRATES  4

#endif
