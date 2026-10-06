/*
 * render.h - the wavetable renderer: voices, envelopes, mixing.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * Takes MIDI events and an sf2_file, produces 16-bit stereo PCM.
 * Knows nothing about where the events came from or where the PCM
 * goes - the harness feeds it a .mid and writes a .wav; the synth
 * proper will feed it /dev/vmidi and write /dev/dsp.
 *
 * INTEGER ONLY. The target is a Pentium III (Coppermine) and the
 * arithmetic below is integer for the reasons in that comment - NOT
 * because the machine lacks an FPU. See the note on RENDER_MAX_VOICES, and design/p3-midi-plan.md section 5
 * settles that the synth is userspace precisely so this arithmetic
 * does not live in the kernel.
 *
 * REFERENCES, read not copied, both GPL-2:
 *   softoss_rs.c:34-110   the resample loop's shape - linear
 *                         interpolation, per-voice pan, accumulate
 *                         then scale and clip
 *   TiMidity++            fixed-point precision, and an oracle that
 *                         renders the same SF2 to PCM
 */

#ifndef RENDER_H
#define RENDER_H

#include <stdio.h>
#include "sf2.h"
#include "sf2gen.h"
#include "effects.h"

/*
 * 32 VOICES, NOT 10.
 *
 * DoomHangar.mid peaks at 10 and it is the least demanding file in
 * the project: jazz.mid needs 24, bohemian.mid uses all 16 channels.
 * See vmidi/synth/NOTES-test-corpus.md (development notes, not
 * shipped). Sizing this from the file the user
 * happened to test with is exactly the mistake that produces a synth
 * which fails on the second song anyone tries.
 */
/*
 * MASTER GAIN, 0..1024 where 1024 is unity.
 *
 * BOTH REFERENCES RUN WELL BELOW UNITY AND FOR THE SAME REASON: a
 * wavetable sums N voices into one 16-bit output and nothing bounds
 * that sum. fluidsynth defaults to 0.2 and TiMidity to 0.70
 * (DEFAULT_AMPLIFICATION, timidity.h:104).
 *
 * At unity, jazz.mid peaked at 32757 of 32767 - ten counts from
 * clipping on a 24-voice passage, which is not headroom, it is luck.
 * 0.5 keeps the same music at half scale with 6 dB in hand, which is
 * between the two references and errs toward theirs.
 *
 * This is the ONE place a global level belongs. The per-voice chain
 * above it is now spec-and-hardware correct and must not be detuned
 * to compensate for a missing master.
 */
#define RENDER_MASTER 512

/*
 * ..AND SINCE 2026-09-08 THE MASTER IS A RUNTIME VALUE, render_state.
 * master, RENDER_MASTER being its default. The 512 was set from a
 * 24-voice peak at unity on the workstation and two references'
 * defaults, before the mix accumulator was widened and the output
 * store made to saturate and count - it was guarding a wrap that no
 * longer exists - and it was never measured against what matters on
 * the target: the level of a game's sound effects through the same
 * card. On the Acer a one-channel passage of the Doom demo (strings
 * at velocity 57, CC7 70: -45 dBFS peak here, and -44 in fluidsynth)
 * needed the mixer at maximum to hear at all. -g on vmidid and
 * smf2wav sets it, fluidsynth's units: 1.0 is unity, 0.5 is this
 * default, 2.0 the ceiling - the effects' wet pair is 20 bits wide
 * and 2048 x 2^20 is the last product that fits 31 bits. The output
 * store clips and counts whatever a higher gain does not fit.
 */
#define RENDER_MASTER_MAX 2048

/*
 * 64 VOICES, RAISED FROM 32 ON 2026-09-04.
 *
 * 32 was a guess, and it was the outlier
 * against everything else doing this job: fluidsynth defaults to 256
 * (fluid_synth.c:251), the DreamBlaster X2 does 81 in hardware,
 * TiMidity defaults to 64 (timidity.h:113-114).
 *
 * THE MACHINE IS A PENTIUM III (COPPERMINE), not the "233 MHz
 * Pentium II" that several documents and this file used to say. That
 * figure came from an EARLY 86Box configuration and was never the
 * target - and it is not even the emulator today, which is
 * `pentium2_deschutes` at 400 MHz (tests/vm/86box.cfg). Corrected
 * 2026-09-04 from the P3's own kernel banner in tests/logs:
 * "CPU: Intel Pentium III (Coppermine) stepping 03".
 *
 * That matters for more than pedantry: a Coppermine has a fully
 * pipelined FPU, so the integer-versus-float argument for this
 * renderer is NOT the one-sided case a 233 MHz P5 would make. Integer
 * stays because it works and its overflow bugs are already found and
 * paid for, not because the hardware forces it.
 *
 * WHAT SETTLED THE VOICE COUNT is a fact rather than an argument: the
 * user already runs TiMidity at 64 voices with eawpats ON THE P3. The headroom is
 * demonstrated on the target, and ours should be cheaper per voice -
 * TiMidity mixes in floating point, this renderer is integer
 * throughout.
 *
 * MEASURED on gmstriving.mid, the worst case in the corpus:
 *
 *   render time  1.27 s -> 1.37 s, EIGHT PERCENT. The mix loop skips
 *                inactive voices, so cost tracks notes sounding
 *                rather than the limit.
 *   stolen       738 -> 38. bohemian 99 -> 0; jazz and DoomHangar
 *                never steal at either setting.
 *   deviation from an unlimited 96-voice render of the same file:
 *                20.6 -> 0.1, effectively identical.
 *   memory       6.2 KB -> 12.5 KB of voice state, against a sample
 *                pool of 10 MB at minimum.
 *
 * Why that file needs it: its own polyphony is 21, but it wants 87
 * voices - the gap is release tails held by 227 sustain events. See
 * vmidi/synth/NOTES-test-corpus.md.
 *
 * AND THE ORIGINAL WARNING STILL STANDS: DoomHangar.mid peaks at 10
 * and is the LEAST demanding file here. Sizing this from the file the
 * user happened to test with is the mistake that produces a synth
 * failing on the second song anyone tries.
 */
/*
 * THE VOICE ARRAY IS ALLOCATED, NOT FIXED - design/21 section 17,
 * built 2026-10-02. It holds as many voices as the ceiling asks for
 * (render_set_max_voices), so `smf2wav -p 64' costs 64 voices and
 * not 2048, and vmidid can take -p 512 on a machine that can render
 * it.
 *
 * RENDER_MAX_VOICES IS NOW ONLY A BOUND - the most any caller may
 * ask for, and what `smf2wav -p auto' reserves to grow into. It costs
 * nothing unless asked for, so one value serves every program; each
 * front end states its own lower cap (vmidid's VMIDID_MAX_VOICES).
 * Until this it sized a fixed array and smf2wav needed render.c built
 * a second time with a bigger one.
 *
 * RENDER_DEFAULT_VOICES is what render_init() allocates and sets, so
 * a caller that never asks gets exactly what it always got - 64.
 */
#ifndef RENDER_MAX_VOICES
#define RENDER_MAX_VOICES 2048
#endif
#define RENDER_DEFAULT_VOICES 64
#define RENDER_CHANNELS   16
#define RENDER_DRUM_CHAN   9

/*
 * 12 FRACTIONAL BITS IN THE SAMPLE POINTER.
 *
 * softoss uses 9 (softoss_rs.c:48,52); TiMidity uses 12
 * (timidity.h:198). The difference is audible, and 12 is right:
 *
 *   at 9 bits, a note an octave BELOW its root has a step of 256, so
 *   one LSB of truncation is 4.7 CENTS - out of tune by ear.
 *   at 12 bits the same error is 0.59 cents, which is not.
 *
 * The cost is range: 12 bits leaves 19 for the integer part, capping
 * a sample at 524288 frames - 11.9 s at 44100. The longest sample in
 * either soundfont here is 8.5 s, which is only 40% headroom on a
 * file that is not ours.
 *
 * SO THE POINTER IS SAMPLE-RELATIVE, not absolute into the sample
 * pool: it counts from the start of THIS sample, and the cap applies
 * to one sample's length rather than to a 148 MB pool. That removes
 * the limit as a practical concern without giving up the precision.
 */
/*
 * LFO PHASE, 8.24 fixed point, wrapping at LFO_PHASE_ONE; THE VALUE
 * the triangle returns stays 16-bit (LFO_VALBITS), so the three
 * places that multiply it by a cents amount are unchanged.
 *
 * SF2 LFOs are TRIANGLE waves, not sine - the spec says so, and a
 * triangle costs one shift and one subtract per frame against a table
 * lookup and a cache miss. With 64 voices
 * that difference is real.
 *
 * WHY 24 BITS AND NOT 16, 2026-09-08: the phase advances once per
 * frame, so at 16 bits the slowest LFO it can hold is one count per
 * frame, 44100 / 65536 = 0.67 Hz, and everything slower was rounded
 * UP to that. The SC-55 font's Halo Pad asks for 0.1 Hz (freqModLFO
 * -7624 cents), a ten-second filter sweep at Q 30 dB; it ran at
 * 0.67 Hz, a 1.5 s cycle, and the user heard Doom's title fanfare
 * "repeat" a sweep fluidsynth "plays slowly". The spec's range goes
 * to -16000 cents, 0.001 Hz; 24 bits reaches 0.0026 Hz and puts the
 * 8.176 Hz default within 0.02 % (step 3110, 8.1747 Hz) instead of
 * 1.2 % (step 12, 8.075 Hz). The 4 Hz LFOs of spec tests 5 and 6 ran
 * at 3.39 Hz for the same reason (step 5 of 5.94); measured, they run
 * at 4.0 after.
 */
#define LFO_PHASEBITS 24
#define LFO_PHASE_ONE (1L << LFO_PHASEBITS)
#define LFO_VALBITS   16

/*
 * THE FILTER IS FLOAT, AND IT IS THE ONLY FLOAT IN THIS RENDERER.
 *
 * IT WAS FIXED POINT FIRST AND THAT DID NOT WORK. The reasoning for
 * Q12 was sound as far as it went - a biquad is
 * y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2, |a1| approaches 2.0 at a
 * low cutoff, and bounding the SUM rather than one product gives Q14
 * a 32-bit worst case against a signed long's 31, where Q12 fits with
 * one bit spare. All true, and it still produced a filter that did
 * not pass audio.
 *
 * THE PROBLEM IS THE COEFFICIENTS' RANGE, WHICH NO SINGLE Q FORMAT
 * HOLDS. Measured DC gain across the spec's cutoff range, where 4096
 * is unity:
 *
 *     fc 2000 cents ->    0   (silence)
 *     fc 5000       -> 1024   (12 dB down)
 *     fc 8000       -> 3739
 *     fc 11000      -> 4096   (correct)
 *
 * The b terms span about 418,000x from one end of the range to the
 * other while the a terms stay within +/-2. A format with enough
 * headroom for a1 has no precision left for b0 at a low cutoff, where
 * b0 is a very small number that rounds to zero - and a low cutoff is
 * exactly where a low-pass filter is doing something. Raising QBITS
 * fixes the bottom and overflows the top. There is no arrangement
 * that holds both ends, which is what fixed point is bad at: absolute
 * precision, where this needs relative.
 *
 * BOTH REFERENCES USE FLOATING POINT HERE, and only here.
 * fluidsynth's fluid_real_t is double throughout; TiMidity is int32
 * fixed point in its mixer and resampler (mix.c:335, resample.c:47)
 * and reaches for double only in do_lowpass, at load time. Neither
 * arrived at fixed point for a filter.
 *
 * THE COST WAS MEASURED, NOT ASSUMED (Acer, 200M iterations, gcc
 * 2.95.2 with TiMidity's Pentium flags):
 *
 *     biquad   fixed 5.730 s   float 7.260 s (1.3x)   double 10.670 s
 *     envelope fixed 3.870 s   float 4.670 s (1.2x)
 *     interp   fixed 2.460 s   float 24.030 s (9.8x)
 *
 * So float for the biquad and nothing else. The interpolator stays
 * fixed - it is 9.8x and runs for every voice on every frame, where
 * the filter is 1.3x on a minority of voices. And float, not double:
 * double is 1.9x on the Acer for no accuracy this needs.
 *
 * AGAINST THE BUDGET: the whole synth measures 14.1% of the 23219 us
 * block deadline on the Acer at 64 voices, so 1.3x on one stage of
 * some voices moves the mean by a fraction of a point. See
 * design/p3-midi-plan.md and
 * tests/logs/2026-09-04-acer-synth-blocktime/.
 *
 * FILTER_QONE REMAINS, for the Q value alone - resonance arrives from
 * the zone layer in fixed point and is converted once per note-on.
 * FILTER_CLAMP is gone with the integer state: a float biquad cannot
 * wrap the way an integer one does, and the ringing it guarded
 * against was an artefact of the fixed-point scaling rather than of
 * the filter.
 */
#define FILTER_QBITS  12
#define FILTER_QONE   (1L << FILTER_QBITS)

/* The spec's cutoff range, SF2.01 generator 8: 1500..13500 absolute
 * cents, i.e. 19.4 Hz to 19912 Hz. fluidsynth clamps to exactly this
 * (fluid_voice.c:851, FRES_MIN/FRES_MAX). */
#define FILTER_FC_MIN  1500
#define FILTER_FC_MAX 13500

/*
 * THE OUTPUT RATE'S BOUNDS, AND WHY THE CEILING IS 46340 - design/24
 * B2, 2026-09-15.
 *
 * voice_set_pitch splits both the pitch ratio and the sample rate so
 * that no 32-bit product can wrap. Its last term is `rem * sr' with
 * rem < rate and sr < rate, so the bound is rate * rate <= LONG_MAX:
 * 46340 on a 32-bit target, which is what this synth runs on. Above
 * it a high-rate sample would drone again, which is the defect that
 * split fixes.
 *
 * The ceiling costs nothing real: no card in this project runs above
 * 48000, the synth's default is 44100, and the two cards it feeds are
 * an SB16 and an ESS Solo-1. Every tool that takes -r checks these.
 */
#define RENDER_RATE_MIN  8000
#define RENDER_RATE_MAX 46340

#define RENDER_FRACBITS 12
#define RENDER_FRACONE  (1L << RENDER_FRACBITS)
#define RENDER_FRACMASK (RENDER_FRACONE - 1)

typedef struct {
    int            active;
    int            channel;
    int            key;            /* the note as played */
    int            velocity;

    const short   *data;           /* into the loaded sample pool */
    long           len;            /* frames */
    long           loop_start;
    long           loop_end;
    int            looping;        /* sampleModes: 1 or 3 */
    int            loop_to_end;    /* mode 3: loop until release */

    long           pos;            /* RENDER_FRACBITS fixed point */
    long           step;

    /* volume envelope, in the SF2's own units where it helps */
    int            stage;
    long           env_level;      /* 0..RENDER_ENV_ONE */
    long           env_rate;
    long           env_count;
    long           delay_frames, attack_frames, hold_frames;
    long           decay_frames, release_frames;
    long           sustain_level;

    /*
     * THE TWO LFOs AND THE MODULATION ENVELOPE.
     *
     * SF2 gives each voice a vibrato LFO (pitch only), a modulation
     * LFO (pitch, volume and filter) and a second six-stage envelope.
     * Without them a note plays at the correct pitch and volume and
     * does not MOVE - held notes sit flat, which is heard as "the
     * soundfont sounds lifeless" rather than as a missing feature.
     *
     * Phase is 16.16 turning over at LFO_PHASE_ONE, so the triangle
     * is a shift and a subtract rather than a table lookup.
     */
    long           vib_phase, vib_step, vib_delay;
    long           mod_phase, mod_step, mod_delay;
    int            vib_to_pitch;      /* cents at full deflection */
    int            mod_to_pitch;      /* cents */
    int            mod_to_volume;     /* centibels */

    int            menv_stage;
    /*
     * THE MODULATION ENVELOPE CARRIES SUB-BITS, like the volume one.
     * menv_sub is menv_level << ENV_SUBBITS and is what actually
     * moves; menv_level is derived from it. Without the extra bits
     * `rate = range / frames' underflows to zero for anything longer
     * than ENV_ONE frames - 1.49 s at 44100 - and the envelope
     * freezes. See menv_step.
     */
    long           menv_level;
    long           menv_sub;
    long           menv_rate;        /* in ENV_SUBBITS units */
    long           menv_count;
    long           menv_delay_frames, menv_attack_frames;
    long           menv_hold_frames,  menv_decay_frames;
    long           menv_release_frames;
    long           menv_sustain_level;
    int            menv_to_pitch;     /* cents at full envelope */
    /*
     * THE FAST MODE'S CATCH-UP - design/54 D28, 2026-10-05. In
     * RENDER_MENV_FAST a voice whose envelope modulates nothing does not
     * step it; these say how far behind it is: frames skipped since the
     * note started, and after how many of them the note was released
     * (-1: not while frozen). render.c's menv_catch_up() replays them
     * the moment the envelope starts to matter.
     */
    long           menv_skipped;
    long           menv_rel_at;

    /*
     * THE RESONANT LOW-PASS, one per voice.
     *
     * Its cutoff moves with modLfoToFilterFc and modEnvToFilterFc, so
     * it cannot be baked into the sample at load time the way
     * TiMidity does it (sndfont.c:684) - it has to run per frame.
     */
    int            filter_on;         /* 0 when the zone sets no cutoff */
    long           fc_cents;          /* the zone's nominal cutoff     */
    long           filter_q;          /* Q in FILTER_QBITS fixed point */
    float          f_b0, f_b1, f_a1, f_a2;   /* b2 == b0 for a low-pass */
    float          f_x1, f_x2, f_y1, f_y2;
    int            gain;              /* attenuation x velocity, 0..GAIN_ONE */
    int            zone_pan;          /* the zone's own pan, -500..500 */
    long           env_sub;           /* env_level << ENV_SUBBITS,
                                      * the decaying state - see
                                      * env_decay_step */
    long           f_last_fc;         /* so coefficients are recomputed
                                       * only when the cutoff moves    */
    int            mod_to_fc;         /* cents at full LFO deflection  */
    int            menv_to_fc;        /* cents at full envelope        */

    int            left_vol, right_vol;   /* 0..GAIN_ONE */

    /*
     * THE MODULATOR LAYER'S PER-VOICE STATE - design/18 step 3.
     *
     * `mods' is the zone's merged list, copied at note-on, so that a
     * controller, bend or pressure event can re-sum any destination
     * for this voice alone. `gen_base' is the zone's generator values
     * BEFORE the modulators were summed in, so a re-sum is
     * base + new sum and never a re-resolve of the zone. The three
     * bases below are what the live setters rebuild from:
     * attenuation already scaled by the x0.4 the generator alone
     * gets, the pitch in cents from the generators alone, and the
     * sample's own rate for the step. This is fluid_voice_modulate()'s
     * data, in our units: fluidsynth keeps the same list and base per
     * voice for the same reason.
     */
    /*
     * THE EFFECT SENDS - design/21. send_reverb/send_chorus are the
     * zone's generators 16/15 plus their modulators (CC91/93 defaults
     * 8.4.8/8.4.9 and a font's own), 0..GAIN_ONE for 0..100 %, set at
     * note-on and re-summed live like every other destination. The
     * *_gain pair is gain x send, precomputed whenever either moves,
     * so the mix loop pays one product per bus per frame and none at
     * all when both are zero.
     */
    int            send_reverb, send_chorus;      /* 0..GAIN_ONE */
    long           send_reverb_gain, send_chorus_gain;
    sf2_modlist    mods;
    short          gen_base[SF2_GEN_COUNT];
    long           att_base;              /* cB, x0.4 applied          */
    long           pitch_base;            /* cents, generators only    */
    long           sample_rate;
    int            root_key;
    int            exclusive_class;
    int            held;           /* sustain pedal is holding it */
    int            shed;           /* its release was shortened by
                                    * render_shed_tails()          */
    unsigned long  start_order;    /* for oldest-voice stealing */
} render_voice;

typedef struct {
    int program;
    int bank;
    /*
     * A DRUM CHANNEL, not "channel 10". Channel 10 starts as one and
     * a system reset makes it one again; a GS "use for rhythm part"
     * SysEx or, in XG style, a bank MSB of 120/126/127 makes any
     * channel one (and back). Drums live in bank 128 + MSB - the
     * offset a stacked font adds is what lets an AWE32-era song's
     * "CC 0 = 1" on its drum channel reach the song font's own kit
     * (fluidsynth's rule, measured on Uplift). design/22 step 2.
     */
    int drum;
    int volume;        /* CC 7   */
    int expression;    /* CC 11  */
    int pan;           /* CC 10  */
    int modwheel;      /* CC 1   */
    int sustain;       /* CC 64  */
    int bend;          /* -8192..8191 */
    int bend_range;    /* semitones, from RPN 0 */
    int rpn;           /* the selected RPN, -1 if none */

    /*
     * EVERY CONTROLLER'S LAST VALUE, for the modulator layer. The
     * named fields above stay - the per-frame paths read them - and
     * this is the general store the modulator sources read: a font
     * may name CC 21-24, 91, 93 or any other legal controller, and
     * design/18 section 3b found sixty that do. Set beside the named
     * field on every CC, reset with them on CC 121.
     */
    unsigned char cc[128];

    /*
     * CHANNEL AND POLYPHONIC PRESSURE, sources 13 and 10 of the
     * modulator model (8.2.1). Untracked until step 3: 0xD0 had no
     * target, so default 8.4.3 - aftertouch to vibrato, 50 cents -
     * and the survey's 583 pressure modulators read a constant 0.
     * Poly pressure is per key, so it is stored per key and only the
     * voices on that key are re-evaluated.
     */
    int           pressure;
    unsigned char poly[128];

    /*
     * NRPN - design/22 step 3. Two families, selected by CC 99:
     *
     *   120  the SF2 spec's own (section 9.6): CC 98 names a generator
     *        (100/101/102 add 100/1000/10000 first), the data entry
     *        is an OFFSET added to the zone's value, scaled per
     *        generator (nrpn_scale[]), applied on the data MSB.
     *   127  the AWE32's: CC 98 names one of 27 parameters, converted
     *        into SF2 units and REPLACING the zone's value for every
     *        note after it (fluidsynth's override_gen_default), the
     *        realtime ones re-applied to sounding voices at once.
     *
     * Both are per channel and cleared by CC 121 and a system reset,
     * as fluidsynth's init_ctrl clears them.
     */
    int           log_font, log_preset;  /* last resolved, for r->log */
    int           nrpn_active;    /* a CC 99/98 since the last RPN select */
    int           nrpn_hundreds;  /* SF2: the +100s accumulated so far */
    int           nrpn_gen;       /* SF2: the selected generator, -1 none */
    short         nrpn_add[SF2_GEN_COUNT];
    short         awe_val[SF2_GEN_COUNT];
    unsigned char awe_set[SF2_GEN_COUNT];
} render_channel;

/*
 * THE MIX ACCUMULATES IN LONGS, RENDER_MIX_FRAMES AT A TIME.
 *
 * render_mix() accepts any frame count and walks it in sub-blocks of
 * this size, so the accumulator is a fixed 8 KB in render_state
 * rather than sized to the caller's request (vmidid's -b allows up
 * to 8192). Sub-blocking changes nothing audible: every per-block
 * quantity - channel volume, the bend step, the loop condition -
 * depends only on state that changes BETWEEN render_mix calls, never
 * within one, so it recomputes to the same value.
 */
#define RENDER_MIX_FRAMES 1024

/*
 * THE VOICE GAIN'S FIXED POINT: 0..GAIN_ONE, GAIN_ONE being unity.
 *
 * WIDENED FROM 10 BITS TO 15 ON 2026-09-08. At 1024, one LSB was
 * -60.2 dB and anything quieter was 0; the old velocity curve never
 * got near it, but a soundfont's 144 dB velocity curve does, and so
 * does a linear 96 dB one - both are spec-test instruments (13B,
 * 13D) and both went SILENT below velocity 47 where fluidsynth reads
 * -60 to -83 dB (design/18 section 10g). One LSB is now -90.3 dB.
 *
 * WHY 15 AND NOT 16. The sample after the filter can reach 17 bits -
 * a resonant low-pass rings above its input, which is why the
 * envelope multiply in render_mix is split in two - and 17 + 16 would
 * overflow the target's 32-bit long. 17 + 15 = 32 does too, so the
 * two gain multiplies are split the same way. RENDER_MASTER keeps its
 * own 10-bit scale; it is applied after the gain and is not part of
 * this.
 */
#define GAIN_BITS 15
#define GAIN_ONE  (1L << GAIN_BITS)

/*
 * FONTS ARE STACKED, NOT MERGED - design/22 step 1, the AWE32's and
 * fluidsynth's shape. Each font keeps its own sample pool and a bank
 * offset: `sfxload -b 1 song.sf2' appended a font's presets to bank 1
 * and fluidsynth's `load FILE 1 1' does the same, and the AWE32-era
 * songs (vmidi/refs/awe32-midi-conversions/, 106 of 112) are written
 * for exactly that - a GM font in bank 0, the song's own in bank 1.
 * A lookup asks each font for (bank - offset, program), LATER FONTS
 * FIRST so the specific overrides the general, then falls back the
 * way it always did (find_preset). A voice remembers its font's pool
 * by pointer, so nothing in the mix loop knows there is more than one.
 */
#define RENDER_MAX_FONTS 8

typedef struct {
    const sf2_file *sf;
    const short    *pool;          /* the font's whole smpl chunk, 16-bit */
    long            pool_frames;
    int             bank_offset;   /* MIDI bank = font bank + this */
} render_font;

/*
 * BANK-SELECT STYLE, set by the GM / GS / XG resets (render_sysex)
 * and read on every CC 0 and CC 32 - fluidsynth's synth.midi-bank-
 * select, default gs, which is also what the SC-55 does:
 *
 *     gm   CC 0 is the bank, as under gs; CC 32 ignored. NOT fluidsynth's
 *          gm, which ignores CC 0 - the AWE32 kernel driver accepts bank
 *          select in GM mode ("normal bank controls; accept both MSB
 *          and LSB", awe_wave.c midi_select_bank), and on this target
 *          every play through the OSS sequencer begins with a GM On:
 *          measured 2026-09-08 on the Acer, where fluidsynth's rule
 *          left an AWE32 song's stacked font unreachable for the whole
 *          run. GM On still resets every channel. design/22 section 13.
 *     gs   CC 0 is the bank (128 + MSB on a drum channel); CC 32 ignored
 *     xg   CC 0 = 120/126/127 makes the channel drums, anything else
 *          makes it melodic and is otherwise ignored; CC 32 is the bank
 *
 * design/22 section 5 has the table read from fluid_chan.c.
 */
#define RENDER_BANK_GM 0
#define RENDER_BANK_GS 1
#define RENDER_BANK_XG 2

typedef struct {
    render_font     fonts[RENDER_MAX_FONTS];
    int             nfonts;
    int             bank_style;    /* RENDER_BANK_* */
    long            master;        /* 0..RENDER_MASTER_MAX, 1024 unity */

    int             rate;          /* output sample rate */

    /*
     * THE MODULATOR MERGE FLAGS, passed to sf2_zone_find_all_m() on
     * every note - today only the velocity-filter default's mode
     * (SF2_MOD_VF_*, sf2mod.h). Set by render_set_velfilter(); the
     * front ends' -F option. AWE unless told otherwise.
     */
    unsigned        modflags;

    /*
     * WHAT THE MODULATOR LAYER DID, cumulative - for the -v summaries.
     * `evaluated' went through mod_eval() into a voice parameter, at
     * note-on or on a controller event; `unimplemented' target reverb
     * or chorus, which do not exist yet (design/18 section 2, the
     * blocked 1162 - the apply table gains two rows when they do).
     */
    unsigned long   mods_evaluated;
    unsigned long   nrpns;         /* NRPN data entries acted on */
    unsigned long   nrpns_ignored; /* ..and not: unknown family/parameter */

    /*
     * THE EFFECTS STAGE - design/21. Two mono send buses accumulated
     * in the voice loop beside the dry pair, processed once per frame
     * after it, summed into acc[] before the one saturating store.
     * `effects' is the switch (-E on|off): off, the buses are neither
     * cleared nor fed and the render is what it was before the stage
     * existed - the guard that it costs nothing unused.
     *
     * REVERB AND CHORUS ARE SEPARATE SINCE 2026-10-05 (the user: "build
     * the reverb chorus split"). fx_reverb and fx_chorus each gate their
     * own bus, sends and block; `effects' is now DERIVED - either of them
     * on - and gates the wet stage as a whole, so both off is still the
     * render from before the stage existed and both on is unchanged.
     * The reverb is nearly all of the cost (design/21: the chorus is
     * ~2.5% of the 23%), so reverb off with chorus on is the cheap way
     * to keep some width on a slow machine. Set them with the setters
     * below, which keep `effects' in step.
     */
    int             effects;
    int             fx_reverb;
    int             fx_chorus;
    /*
     * THE FILTER BYPASS, design/20 section 3b, off unless asked. At
     * note-on, a voice whose cutoff nothing modulates, whose Q is at
     * or below the no-hump default, and whose cutoff in Hz sits at or
     * above the highest frequency its resampled sample can carry
     * (half the output rate times the playback ratio) gets no filter:
     * the biquad would only add phase shift there. 82 % of
     * gmstriving's voice-frames on the SC-55 go through the filter
     * (synthcost, the Acer, 2026-09-08); the audit measured the rule
     * at 94 % -> 63 % of frames and -14 % of render time on the
     * workstation, worst per-block level change 0.17 dB. A switch so
     * the target can measure it both ways before it is decided.
     */
    int             filter_bypass;
    /*
     * THE MODULATION ENVELOPE'S MODE - design/54 D28, 2026-10-05 (the
     * user: an option that speeds rendering and one "more correct and to
     * spec"). RENDER_MENV_REFERENCE (0, the default) steps every voice's
     * envelope every frame, as the spec describes it. RENDER_MENV_FAST
     * skips it while neither depth is set - design/20's 7% - and catches
     * it up exactly when one becomes non-zero; the two render
     * byte-identically, which is how FAST is proven.
     */
    int             menv_mode;
    /*
     * THE FILTER'S FLOAT-TO-LONG CONVERSION IS THE ROUNDING ADD, NOT
     * THE CAST - decided 2026-09-09 from the Acer's run 32: the cast's
     * two control-word loads per filtered frame cost that Pentium III
     * 6.7 points of the deadline, 11 % of the renderer, and removing
     * them took gmstriving at 64 voices from 3319 blocks over budget
     * to 139. The add rounds to nearest where the cast truncated toward
     * zero: per filtered sample at most 1 LSB, after the effects at
     * most 9 of 32767 on any font here, 65-88 dB below the music, per-
     * block level 0.00 dB, the spec test's deviation from fluidsynth
     * unchanged (design/20 3b-ii, and the ten-piece comparison of
     * 2026-09-09). The rounded result is the closer one.
     *
     * The cast is kept under RENDER_CONVERSION_CAST, off, for ONE
     * reason: a build with it reproduces every render made before
     * 2026-09-09 bit for bit, so the earlier baselines, bisect
     * binaries and listen sets can be re-derived from the tree. It is
     * not a choice anyone should make on a target. The denormal guard
     * that was measured beside it (run 32: +3.2 points, never changed
     * a sample) is deleted, not kept.
     *
     * The rule: a compile-time alternative exists to reproduce a
     * measured past, not to offer a choice; a choice is a runtime
     * value; a superseded path is deleted.
     */
    long            bus_reverb[RENDER_MIX_FRAMES];
    long            bus_chorus[RENDER_MIX_FRAMES];
    /* The two effects' summed wet pair for one block - they ADD into
     * it, so the master is applied once over the sum (design/20 10b). */
    long            wet_l[RENDER_MIX_FRAMES];
    long            wet_r[RENDER_MIX_FRAMES];
    /* The reverb's delay lines, ~123 KB of longs - effects.h. Inside
     * the state so one render_init sets everything a render needs. */
    reverb_state    reverb;
    chorus_state    chorus;             /* 8 KB, effects.h */

    /*
     * THE ACTIVE VOICE LIMIT, 1..RENDER_MAX_VOICES, and never more
     * than `nalloc'.
     *
     * THE ARRAY IS ALLOCATED TO FIT - design/21 section 17. This
     * comment used to say a fixed array saved a malloc in a path that
     * had none, which was true at 64 voices and stopped being true
     * when smf2wav's 2048 made every render carry 2.3 MB. The malloc
     * is still never in the hot path: it happens in render_init(),
     * render_set_max_voices() and render_set_grow(), all between
     * blocks, and alloc_voice() never allocates.
     *
     * WHY IT IS A RUNTIME VALUE. Sizing polyphony is a measurement,
     * and the measurement wants several answers from one run: the
     * 86Box block-size sweep took seven boots to say that -b and -q
     * do not matter, and the same question about voices should not
     * cost four rebuild-and-restage cycles. It also lets the SAME
     * BINARY answer it on the P3, where the number actually matters.
     *
     * render_init sets it to RENDER_MAX_VOICES, so a caller that
     * never touches it behaves exactly as before.
     */
    int             max_voices;

    render_voice   *voices;         /* nalloc long, render_free()s  */
    int             nalloc;

    /* Grow rather than steal - render_set_grow(). Offline only. */
    int             grow;

    /* How high grow mode has taken max_voices, for reporting. 0 when
     * it never grew, so a caller can tell "did not need to" from
     * "was not allowed to". */
    int             grew_to;
    render_channel  chan[RENDER_CHANNELS];
    unsigned long   order;         /* increments per note-on */

    /* counters, so a run can be checked rather than trusted */
    unsigned long   notes_started;
    unsigned long   notes_stolen;
    unsigned long   tails_shed;    /* render_shed_tails(), total */
    unsigned long   notes_unmapped;
    unsigned long   clipped;       /* samples saturated in the mix */
    /* Voice-frames rendered, and how many of them went through the
     * filter - per-voice per-block accounting, one add each, so
     * tests/synthcost.c can print the filter's share on the target
     * (design/20 section 3, the bypass's stake). */
    unsigned long   voice_frames;
    unsigned long   filtered_frames;
    unsigned long   voices_bypassed;   /* note-ons the bypass rule took */
    unsigned long   menv_catchups;     /* envelopes FAST caught up */

    /*
     * The mix accumulator - see RENDER_MIX_FRAMES. Longs so that 64
     * voices cannot wrap it; the ONE saturating store to the caller's
     * short buffer is where clipping happens and is counted.
     */
    long            acc[RENDER_MIX_FRAMES * 2];

    /*
     * ONE VOICE'S BLOCK, before it is panned and sent - design/20
     * 10b candidate 1, 2026-09-09.
     *
     * The frame loop used to finish each frame by multiplying the
     * sample into FOUR arrays: left, right, the reverb bus and the
     * chorus bus. Four multiplies and four read-modify-writes to four
     * different places, per voice per frame. Every coefficient in
     * them - left_vol, right_vol, send_reverb_gain, send_chorus_gain -
     * is written only by voice_set_pan/gain/sends, which run at
     * note-on and on controller events BETWEEN blocks; nothing
     * inside the frame loop touches them. So they are constant for
     * the block, and the same products can be formed afterwards in
     * streaming passes over this buffer: one multiply per frame per
     * destination, sequential, and a destination whose coefficient is
     * zero skipped entirely (on the SC-55 that is both sends on most
     * voices).
     *
     * The products are the same values in the same order, so the
     * output is BIT-IDENTICAL - that is the acceptance, not a
     * tolerance. TiMidity has always worked this way (mix.c); it is
     * also what MMX would need, since the passes are the only part of
     * the loop that vectorises and the x87 filter cannot sit beside
     * MMX without an EMMS per frame.
     *
     * One buffer, not one per voice: voices are rendered in turn.
     */
    long            vbuf[RENDER_MIX_FRAMES];

    /*
     * VOICE TRACE, off unless a caller sets it. Called once per voice
     * after every generator has been resolved, so what it prints is
     * what the voice will actually play.
     *
     * It exists because the alternative is ESTIMATING the envelope
     * from the rendered audio, which an outside review proposed and
     * which is wrong twice over: it would infer, with error, values
     * held here exactly, and its 4-stage ADSR is not the 6-stage SF2
     * DAHDSR that both this synth and fluidsynth implement. See
     * tests/compare/STRATEGY-REVIEW.md.
     */
    void          (*trace)(const render_voice *v, void *arg);
    void           *trace_arg;

    /*
     * WHAT THE RENDER DID, NOT WHAT IT WAS GIVEN - design/22 section
     * 13. When non-NULL, one line per change of a channel's resolved
     * font and preset, and one per SysEx acted on or ignored. The
     * daemon's -v sets it; the Acer's two-font run of 2026-09-08 had
     * every argument right, every font loaded, and no way to see that
     * a GM On had made the second font unreachable.
     */
    FILE           *log;
} render_state;

/*
 * ALLOCATES RENDER_DEFAULT_VOICES VOICES. If that fails, max_voices is
 * 0 and nothing will sound - a caller that cares checks it (vmidid and
 * smf2wav do). Still void, so the test tools that call it, and the
 * parser test's stub of it, are unchanged.
 */
void render_init(render_state *r, const sf2_file *sf, const short *pool,
                 long pool_frames, int rate);

/* FREE THE VOICE ARRAY. NULL-safe and idempotent; the state may be
 * render_init()ed again afterwards. */
void render_free(render_state *r);

/*
 * Stack another font on top of the ones already loaded, its banks
 * offset by `bank_offset' (0..127). Returns its index, or -1 when
 * RENDER_MAX_FONTS are loaded. render_init loads the first font at
 * offset 0; a font added later is searched BEFORE the ones below it.
 */
int  render_add_font(render_state *r, const sf2_file *sf, const short *pool,
                     long pool_frames, int bank_offset);

/*
 * Set the voice ceiling. Clamped to 1..RENDER_MAX_VOICES and
 * returns what was actually set, so a caller can report it rather
 * than assume. RAISING IT PAST WHAT IS ALLOCATED GROWS THE ARRAY; if
 * that allocation fails the ceiling stops at what there is, and the
 * return value says so. Call it after render_init, between blocks:
 * lowering it mid-render does not stop voices already sounding, it
 * only stops new ones being allocated above the new limit.
 */
int  render_set_max_voices(render_state *r, int n);

/*
 * SHED RELEASE TAILS - TiMidity's first move when its buffer drains
 * (design/21 section 16): up to `n' voices already in their release,
 * QUIETEST FIRST, drum channels left alone, are faded out over 10 ms
 * instead of finishing their tail. A fade, not a cut, so nothing
 * clicks. Held notes are never touched. Returns
 * how many were shed. Call between blocks.
 */
int  render_shed_tails(render_state *r, int n);

/* How many voices are sounding, and how many of those are HELD - in
 * attack, decay or sustain, or kept by the pedal - rather than in
 * their release. The difference is what render_shed_tails() may take. */
void render_voice_counts(const render_state *r, int *active, int *held);

/*
 * GROW THE CEILING INSTEAD OF STEALING - for OFFLINE rendering only.
 *
 * With this set, alloc_voice() raises max_voices when it would
 * otherwise steal, up to RENDER_MAX_VOICES. The render then ends at
 * the file's TRUE voice demand, which is a thing worth knowing and
 * which nothing else reports: the corpus scan of 2026-09-19 had to
 * rebuild at successively higher ceilings to find that one file
 * wanted 409 (design/21 section 15).
 *
 * OFF BY DEFAULT, AND THAT IS NOT CAUTION. Every comparison in
 * design/20 and design/21 depends on smf2wav being deterministic at
 * a STATED ceiling; a renderer that quietly grew would make two runs
 * of the same file differ and make old measurements unreproducible.
 *
 * NEVER FOR vmidid. A live synth has a deadline, and growing past
 * what the machine can render is the opposite of what it needs -
 * that wants auto-REDUCTION (design/21 section 16).
 */
void render_set_grow(render_state *r, int on);

/*
 * Which velocity-filter default to play: SF2_MOD_VF_AWE (the default),
 * _201, _204 or _NONE - design/18 section 10f - OR'd with the volume
 * law, SF2_MOD_LAW_SPEC (default) or _LINEAR (design/22 section 16).
 * Takes effect on the next note-on; sounding voices keep what they
 * resolved with.
 */
void render_set_velfilter(render_state *r, unsigned modflags);

/* Reverb and chorus on or off together - design/21. Default on. */
void render_set_effects(render_state *r, int on);

/* One of them on or off, the other untouched (2026-10-05). */
void render_set_reverb(render_state *r, int on);
void render_set_chorus(render_state *r, int on);

/* THE -E WORDS, SHARED by vmidid and smf2wav so the two cannot drift:
 * `on' both, `off' neither, `reverb' or `chorus' that one alone. A
 * mask of RENDER_FX_* - parse returns -1 for anything else. */
#define RENDER_FX_REVERB 1
#define RENDER_FX_CHORUS 2
#define RENDER_FX_BOTH   (RENDER_FX_REVERB | RENDER_FX_CHORUS)
int         render_fx_parse(const char *word);
const char *render_fx_name(int mask);
void        render_set_fx(render_state *r, int mask);

/* The filter bypass rule (render_state.filter_bypass) on or off.
 * Default off. Takes effect at the next note-on. */
void render_set_filter_bypass(render_state *r, int on);

/* The modulation envelope's mode (render_state.menv_mode) - design/54
 * D28. Safe to change between blocks: a switch to REFERENCE catches up
 * any skipped envelope on its next frame. */
#define RENDER_MENV_REFERENCE  0
#define RENDER_MENV_FAST       1
void render_set_menv_mode(render_state *r, int mode);

/* The master gain in 1/1024ths, clipped to 1..RENDER_MASTER_MAX;
 * RENDER_MASTER (0.5) unless set. Safe to change between blocks. */
void render_set_master(render_state *r, long gain_x1024);

/*
 * CHANGE THE OUTPUT RATE without re-initialising - for a daemon whose
 * output DEVICE changed under it (design/43 section 5b). Takes effect
 * at the next note-on, like the two setters above: `rate' is read
 * when a voice is allocated, never cached.
 *
 * REFUSES anything outside RENDER_RATE_MIN..RENDER_RATE_MAX rather
 * than clamping, and returns the rate in force either way, so a
 * caller handed 48000 by a card learns it cannot render there.
 *
 * IT RE-DERIVES REVERB AND CHORUS, discarding their tails. Call it
 * only when nothing is sounding - render_active() == 0 - or a ringing
 * tail is cut. vmidid's 3000 ms release guarantees that.
 */
int  render_set_rate(render_state *r, int rate);

/* One MIDI channel message. status is 0x80..0xEF. */
void render_event(render_state *r, unsigned char status,
                  unsigned char d1, unsigned char d2);

/*
 * One System Exclusive message, the body only - no F0, no F7. Acts
 * on exactly what fluidsynth 2.5.7 acts on, minus tuning: GM System
 * On (7E dev 09 01), the GS reset (41 dev 42 12 40 00 7F 00 41) and
 * GS "use for rhythm part" (41 dev 42 12 40 1x 15 v chk), the XG
 * reset (43 dev 4C 00 00 7E 00). Device id 0x10 or 0x7F. Everything
 * else is ignored. design/22 section 5.
 */
void render_sysex(render_state *r, const unsigned char *body, int len);

/*
 * The system reset those messages perform: every voice off, every
 * channel to its power-on state (program 0, channel 10 the drum
 * channel, controllers at their defaults), the effects cleared. The
 * bank style is left to the caller.
 */
void render_system_reset(render_state *r);

/*
 * Render `frames' stereo frames into out[2*frames]. Any count is
 * accepted. The sum is exact and saturates at the output, counting
 * each clipped sample in r->clipped.
 */
void render_mix(render_state *r, short *out, int frames);

/*
 * SILENCE EVERYTHING, IGNORING THE SUSTAIN PEDAL.
 *
 * For a LIVE stream that stops without releasing what it was holding
 * - the device closed, SNDCTL_SEQ_RESET, lxdoom's S/RESET control
 * byte. A file renderer does not need it; a daemon does, because
 * every one of those ends the stream at a point the music did not
 * choose. See render_panic's own comment for why the order matters.
 */
void render_panic(render_state *r);

/* How many voices are sounding - for tests and for a status line. */
int  render_active(const render_state *r);

#endif /* RENDER_H */
