/*
 * smf2wav - render a MIDI file through the synth into a WAV.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * THE DEVELOPMENT LOOP. The synth's real chain is
 *
 *     lxdoom -> musserv -> /dev/sequencer -> vmidi -> synth -> vsound
 *
 * every stage of which needs the P3 powered up and a bundle staged.
 * This runs the middle two on the workstation against a file, so a
 * change to the renderer can be HEARD in seconds rather than after a
 * trip with a CF card. 86Box and the P3 remain the acceptance test
 * for how it sounds through the real card.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "sf2.h"
#include "sf2gen.h"
#include "smf.h"
#include "render.h"

/*
 * THE DEFAULT CEILING FOR OFFLINE RENDERING, which is NOT the
 * daemon's.
 *
 * 512 covers every file measured - the worst in a 152-file corpus
 * wants 409 (design/21 section 15) - with headroom, and costs under
 * 1 MB of voice state. RENDER_MAX_VOICES is higher still so `-p auto'
 * and a hand-set `-p' have somewhere to go if a file ever exceeds
 * what has been seen.
 *
 * vmidid's default stays 64: it has a deadline and none of the
 * machines this project has can sustain even that.
 */
#define SMF2WAV_DEFAULT_VOICES 512

#define BLOCK 1024

static void
put32(FILE *fp, unsigned long v)
{
    putc((int) (v & 0xff), fp);
    putc((int) ((v >> 8) & 0xff), fp);
    putc((int) ((v >> 16) & 0xff), fp);
    putc((int) ((v >> 24) & 0xff), fp);
}

static void
put16(FILE *fp, unsigned int v)
{
    putc((int) (v & 0xff), fp);
    putc((int) ((v >> 8) & 0xff), fp);
}

/* A canonical 44-byte WAV header. Written twice: once with zero
 * lengths, once with the real ones after the data is known. */
/*
 * THE RUNAWAY CEILING - FOLLOWS THE FILE SINCE 2026-10-04, under a cap
 * in BYTES.
 *
 * It was a flat 1200 s (20 minutes): a guard against a file whose tempo
 * map diverges, or a voice that never releases, filling the disk. It
 * also cut every genuinely longer piece - sonata_b_(c)finley.mid is
 * 25:09 and came out 20:00, reported as a success.
 *
 * NOW: the file's own length + releases + the effects tail + 30 s,
 * never more than RENDER_CAP_BYTES of audio at the chosen rate. A
 * well-formed file never reaches it - the loop stops itself at the
 * file's end, or 2 s after it with a voice stuck. The cap is what stops
 * a bogus length, and the GUI's disk check, fed an honest estimate, is
 * what refuses a render that will not fit.
 *
 * THE CAP IS BYTES, NOT MINUTES, AND UNDER 2 GB ON PURPOSE (a second
 * agent's review): 2.2 with this glibc has no large-file support, so a
 * file cannot grow past 2 GB (a signed off_t), and FAT - which a CF
 * card may carry - has the same limit. The format is 16-bit stereo but
 * the rate runs RENDER_RATE_MIN..MAX, so a time cap would be a different
 * size at every rate; 1.9 GB is about 2 h 50 min at 46340 Hz, 3 h at
 * 44100.
 *
 * The loop must SAY when it stops here - silent until 2026-09-22, which
 * made a truncated render look like a finished one.
 */
/*
 * EXIT CODES - 2026-10-04. 2 MEANS ONLY "STOPPED AT THE CEILING": the
 * Render page reads 2 as that, keeps the (valid, shorter) WAV and says
 * the file is incomplete. Every argument error exited 2 as well, so a
 * bad -r or a missing argument would have been reported as the ceiling,
 * with an empty WAV kept. They exit 64 now - EX_USAGE, the conventional
 * "the command was used wrongly" - and the GUI shows smf2wav's own line
 * for any code it does not know. 1 is any other failure; 0 success.
 */
#define SMF2WAV_EXIT_USAGE  64

/*
 * HALF THE MACHINE'S MEMORY FOR THE MIDI FILE - 2026-10-04, smf.c has
 * why. MemTotal from /proc/meminfo (2.2 has the line, after its older
 * table), capped at 1.5 GB - a 32-bit process cannot address much more.
 * VLHE_SMF_MEMLIMIT (bytes) overrides it for a test. Unreadable: the cap.
 */
static unsigned long
midi_memory_limit(void)
{
    const unsigned long cap = 1536UL * 1048576UL;
    const char *env = getenv("VLHE_SMF_MEMLIMIT");
    unsigned long kb = 0, lim;
    char line[128];
    FILE *f;

    if (env != NULL && *env != '\0')
        return strtoul(env, NULL, 10);
    f = fopen("/proc/meminfo", "r");
    if (f != NULL) {
        while (fgets(line, sizeof line, f) != NULL)
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1)
                break;
        fclose(f);
    }
    if (kb == 0)
        return cap;
    lim = (kb / 2UL) > cap / 1024UL ? cap : (kb / 2UL) * 1024UL;
    return lim;
}


#define RENDER_CAP_BYTES    1900000000UL
#define RENDER_MARGIN_SEC   30UL

/* The cap in seconds, at this rate. */
static unsigned long
render_cap_sec(int rate)
{
    return RENDER_CAP_BYTES / ((unsigned long) rate * 4UL);
}

/* What the file itself needs: its length, releases, the tail. */
static unsigned long
render_need_sec(const smf_file *smf, int effects)
{
    return (unsigned long) (smf->length_usec / 1000000.0) + 3UL
           + (effects ? 2UL : 0UL);
}

/* The ceiling for this file: what it needs plus a margin, under the cap. */
static unsigned long
render_ceiling_sec(const smf_file *smf, int rate, int effects)
{
    unsigned long c = render_need_sec(smf, effects) + RENDER_MARGIN_SEC;
    unsigned long cap = render_cap_sec(rate);

    return c < cap ? c : cap;
}

/*
 * EVERY WRITE IS CHECKED FROM HERE ON, AND THAT IS NEW.
 *
 * Until 2026-09-22 this program discarded fwrite's return in the
 * sample loop, never called ferror(), and did not check fclose() -
 * so a full disk produced a short file, a header rewritten with the
 * frame count it MEANT to write, and `exit 0'. The GUI reported
 * "done" because it was told so.
 *
 * CLAUDE.md section 2 has the same shape twice already: `runtest.sh'
 * printing ALL CHECKS PASSED while skipping a checksum, and
 * `vcdsndtest.sh' degrading past a missing stamp. Both were TEST
 * scripts. This is the first one in a program we ship.
 *
 * One flag, set by the only two functions that write, and read before
 * the header rewrite. Cheaper than checking at every call site and
 * impossible to forget at a new one.
 */
static int g_wr_failed;

static void
wr(const void *p, size_t sz, size_t n, FILE *fp)
{
    if (g_wr_failed)
        return;                 /* already broken - do not pile on */
    if (fwrite(p, sz, n, fp) != n)
        g_wr_failed = 1;
}

static void
wav_header(FILE *fp, int rate, unsigned long frames)
{
    unsigned long data = frames * 4UL;      /* 2 ch, 16 bit */

    wr("RIFF", 1, 4, fp);
    put32(fp, 36UL + data);
    wr("WAVEfmt ", 1, 8, fp);
    put32(fp, 16UL);
    put16(fp, 1);                            /* PCM */
    put16(fp, 2);                            /* stereo */
    put32(fp, (unsigned long) rate);
    put32(fp, (unsigned long) rate * 4UL);   /* byte rate */
    put16(fp, 4);                            /* block align */
    put16(fp, 16);                           /* bits */
    wr("data", 1, 4, fp);
    put32(fp, data);
}

/*
 * LOAD THE SAMPLE POOL. FluidR3's is 148 MB, which is why sf2.c
 * records the position rather than reading it - but a renderer needs
 * it in memory, and on this workstation that is affordable. On the
 * target it will have to be mmap'd or streamed, which is a decision
 * for the synth proper, not for a test harness.
 */
static short *
load_pool(FILE *fp, const sf2_file *sf, long *frames_out)
{
    short *pool;
    long   frames = (long) (sf->sample_size / 2);

    pool = (short *) malloc(sizeof(short) * (size_t) frames);
    if (pool == NULL)
        return NULL;
    if (fseek(fp, sf->sample_pos, SEEK_SET) != 0) {
        free(pool);
        return NULL;
    }
    if (fread(pool, 2, (size_t) frames, fp) != (size_t) frames) {
        free(pool);
        return NULL;
    }
    *frames_out = frames;
    return pool;
}

static void
warn_cb(int gen, const char *name, int count, void *arg)
{
    (void) arg;
    fprintf(stderr, "  generator %2d %-22s used %d times\n",
            gen, name, count);
}

/*
 * -v: THE REAL ENVELOPE, NOT AN ESTIMATE OF IT.
 *
 * An outside strategy document proposed estimating attack, decay,
 * sustain and release from the rendered audio. That is wrong twice:
 * the values are held exactly in render_voice, and its 4-stage ADSR
 * is not the 6-stage SF2 DAHDSR that this synth and fluidsynth both
 * implement. See tests/compare/STRATEGY-REVIEW.md.
 *
 * Frames are printed as milliseconds because that is what a
 * comparison against another synth is made in. Sustain is printed as
 * the raw level rather than a ratio: ENV_ONE is private to render.c,
 * and inventing a scale here would be one more number to keep in step
 * with the renderer.
 */
static void
trace_voice(const render_voice *v, void *arg)
{
    long rate = *(long *) arg;

    fprintf(stderr,
        "  ch%-2d key %3d vel %3d  D%5ld A%5ld H%5ld Dec%5ld R%5ld"
        "  sus %8ld  vol %5d/%-5d  fc %6ld  %s\n",
        v->channel, v->key, v->velocity,
        v->delay_frames   * 1000L / rate,
        v->attack_frames  * 1000L / rate,
        v->hold_frames    * 1000L / rate,
        v->decay_frames   * 1000L / rate,
        v->release_frames * 1000L / rate,
        v->sustain_level,
        v->left_vol, v->right_vol,
        v->fc_cents,
        v->looping ? (v->loop_to_end ? "loop-to-end" : "looping")
                   : "one-shot");
}

/*
 * -s FILE[@BANK]: split the bank offset off a font argument. A `@'
 * in a font's own name would need quoting the other way round; none
 * here has one. Returns -1 on a bad bank.
 */
static int
font_arg(const char *arg, char *name, size_t namelen, int *bank)
{
    const char *at = strrchr(arg, '@');
    size_t n;

    *bank = 0;
    if (at != NULL) {
        char *end;
        long  b = strtol(at + 1, &end, 10);
        if (*end != '\0' || end == at + 1 || b < 0 || b > 127)
            return -1;
        *bank = (int) b;
        n = (size_t) (at - arg);
    } else {
        n = strlen(arg);
    }
    if (n == 0 || n >= namelen)
        return -1;
    memcpy(name, arg, n);
    name[n] = '\0';
    return 0;
}

int
main(int argc, char **argv)
{
    sf2_file      sf;
    /* Fonts stacked above the first by -s FILE[@BANK] - render.h. */
    sf2_file      xsf[RENDER_MAX_FONTS];
    const char   *xname[RENDER_MAX_FONTS];
    int           nx = 0;
    smf_file      smf;
    render_state  r;
    FILE         *sfp, *mfp, *out;
    short        *pool;
    long          pool_frames;
    short         block[BLOCK * 2];
    unsigned long frames = 0, tail, after = 0;
    unsigned long ceiling_sec = 0;    /* render_ceiling_sec(), below */
    int           rate = 44100, ev = 0, i, quiet = 0, verbose = 0;
    unsigned      velfilter = SF2_MOD_VF_AWE;
    unsigned      law       = SF2_MOD_LAW_SPEC;
    int           effects = RENDER_FX_BOTH;   /* -E, a RENDER_FX_* mask */
    int           bypass  = 0;
    int           menv    = RENDER_MENV_FAST;   /* -M; fast by default since the corpus proof, design/54 D28 */
    long          master = 0;         /* -g in 1/1024ths, 0 = default */
    long          trace_rate;
    unsigned long peak = 0;
    int           maxvoices = 0;      /* the peak OBSERVED, not a limit */
    int           want_voices = 0;    /* -p N, 0 = leave the default    */
    int           grow = 0;           /* -p auto                        */
    int           ceiling_hit = 0;    /* the runaway guard fired        */
    int           estimate = 0;       /* -n: say the size and stop      */

    if (argc < 4) {
        fprintf(stderr,
            "usage: smf2wav [-n] [-q] [-v] [-p N|auto] [-r RATE] [-F MODE] [-L LAW]\n"
            "               [-E on|off|reverb|chorus] [-B on|off]\n"
            "               [-M fast|reference] [-g GAIN]\n"
            "               [-s FILE.sf2[@BANK]]... FILE.sf2 FILE.mid OUT.wav\n"
            "  -n  do not render: print the ESTIMATED output size and\n"
            "      duration and exit. Only -r and -E change it.\n"
            "  -s  stack another soundfont above FILE.sf2, its banks\n"
            "      offset by BANK (default 0): -s song.sf2@1 puts the\n"
            "      song's bank 0 in MIDI bank 1. Later fonts are searched\n"
            "      first. Up to %d fonts. design/22 step 1.\n"
            "  -p  max voices: 1 to %d, default %d. `-p auto' GROWS the\n"
            "      ceiling instead of stealing, ending at the file's true\n"
            "      demand - offline only, and off by default because every\n"
            "      comparison in design/20 and 21 needs a stated ceiling.\n"
            "  -v  print every voice's resolved DAHDSR envelope, pan\n"
            "      and filter cutoff as it starts\n"
            "  -L  VOLUME CURVE - a different setting from -F:\n"
            "        spec (default) SoundFont standard, (v/127)^2;\n"
            "        linear gentler, Sound Blaster era, v/127\n"
            "      design/22 section 16.\n"
            "  -F  velocity-filter default: awe (default), 2.01, 2.04,\n"
            "      none - or 1, 2, 3, 0. See design/18 section 10f.\n"
            "  -E  reverb and chorus: on (default), off, or reverb or\n"
            "      chorus for that one alone. design/21.\n"
            "  -B  filter bypass on|off (default off): skip the low-pass where\n"
            "      the cutoff is above anything the sample carries. design/20 3b.\n"
            "  -M  modulation envelope: fast (default) skips it while it\n"
            "      modulates nothing and catches it up exactly when it starts\n"
            "      to, ~7%% less work; reference steps it every frame, as the\n"
            "      spec does. Identical on everything tested. design/54 D28.\n"
            "  -g  master gain: 1.0 unity, 0.5 default, 2.0 max. The output\n"
            "      clips and counts whatever does not fit.\n",
            RENDER_MAX_FONTS - 1, RENDER_MAX_VOICES,
            SMF2WAV_DEFAULT_VOICES);
        return SMF2WAV_EXIT_USAGE;
    }
    while (argc > 4 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-p") == 0 && argc > 5) {
            /* `-p auto' GROWS rather than steals - see render.h. Any
             * other value is a fixed ceiling, as vmidid's -p is. */
            if (strcmp(argv[2], "auto") == 0 || strcmp(argv[2], "a") == 0)
                grow = 1;
            else {
                want_voices = atoi(argv[2]);
                if (want_voices < 1 || want_voices > RENDER_MAX_VOICES) {
                    fprintf(stderr, "smf2wav: -p %s: 1 to %d, or auto\n",
                            argv[2], RENDER_MAX_VOICES);
                    return SMF2WAV_EXIT_USAGE;
                }
            }
            argv += 2; argc -= 2;
        }
        else if (strcmp(argv[1], "-q") == 0) { quiet = 1; argv++; argc--; }
        /*
         * -n: SAY HOW BIG IT WOULD BE AND STOP. Added 2026-09-22 so
         * the GUI can warn before a render that will not fit, without
         * a second copy of the length calculation - smf_load already
         * integrates the tempo map and this is the program that knows
         * what it does with the result.
         *
         * IT OPENS NO OUTPUT AND LOADS NO SOUNDFONT, so it costs a
         * MIDI parse and nothing else. OUT.wav is still required on
         * the command line, so the caller's argv is the same either
         * way and cannot drift.
         */
        else if (strcmp(argv[1], "-n") == 0) { estimate = 1; argv++; argc--; }
        else if (strcmp(argv[1], "-v") == 0) { verbose = 1; argv++; argc--; }
        else if (strcmp(argv[1], "-r") == 0 && argc > 5) {
            rate = atoi(argv[2]); argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-F") == 0 && argc > 5) {
            int f = sf2_mod_vf_parse(argv[2]);
            if (f < 0) {
                fprintf(stderr, "smf2wav: -F %s: awe, 2.01, 2.04 or none\n",
                        argv[2]);
                return SMF2WAV_EXIT_USAGE;
            }
            velfilter = (unsigned) f; argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-L") == 0 && argc > 5) {
            int f = sf2_mod_law_parse(argv[2]);
            if (f < 0) { fprintf(stderr, "smf2wav: -L %s: spec or linear\n", argv[2]); return SMF2WAV_EXIT_USAGE; }
            law = (unsigned) f; argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-E") == 0 && argc > 5) {
            /* A MASK SINCE 2026-10-05 - reverb and chorus apart. Every
             * test below of `effects' asks "is either on", which a
             * non-zero mask answers. */
            effects = render_fx_parse(argv[2]);
            if (effects < 0) { fprintf(stderr, "smf2wav: -E %s: on, off, reverb or chorus\n", argv[2]); return SMF2WAV_EXIT_USAGE; }
            argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-M") == 0 && argc > 5) {
            if (strcmp(argv[2], "fast") == 0)           menv = RENDER_MENV_FAST;
            else if (strcmp(argv[2], "reference") == 0) menv = RENDER_MENV_REFERENCE;
            else { fprintf(stderr, "smf2wav: -M %s: fast or reference\n", argv[2]); return SMF2WAV_EXIT_USAGE; }
            argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-B") == 0 && argc > 5) {
            if (strcmp(argv[2], "on") == 0)       bypass = 1;
            else if (strcmp(argv[2], "off") == 0) bypass = 0;
            else { fprintf(stderr, "smf2wav: -B %s: on or off\n", argv[2]); return SMF2WAV_EXIT_USAGE; }
            argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-g") == 0 && argc > 5) {
            double g = atof(argv[2]);
            if (g < 0.001 || g > 2.0) { fprintf(stderr, "smf2wav: -g must be 0.001..2.0\n"); return SMF2WAV_EXIT_USAGE; }
            master = (long) (g * 1024.0 + 0.5); argv += 2; argc -= 2;
        } else if (strcmp(argv[1], "-s") == 0 && argc > 5) {
            if (nx >= RENDER_MAX_FONTS - 1) {
                fprintf(stderr, "smf2wav: at most %d -s fonts\n",
                        RENDER_MAX_FONTS - 1);
                return SMF2WAV_EXIT_USAGE;
            }
            xname[nx++] = argv[2]; argv += 2; argc -= 2;
        } else break;
    }
    if (argc < 4) { fprintf(stderr, "smf2wav: missing arguments\n"); return SMF2WAV_EXIT_USAGE; }
    /* -r was unbounded until 2026-09-15. The ceiling is the pitch
     * step's (design/24 B2, render.h); the floor keeps the block
     * arithmetic sane. */
    if (rate < RENDER_RATE_MIN || rate > RENDER_RATE_MAX) {
        fprintf(stderr, "smf2wav: -r must be %d..%d\n",
                RENDER_RATE_MIN, RENDER_RATE_MAX);
        return SMF2WAV_EXIT_USAGE;
    }

    /*
     * -n: THE ESTIMATE, AND NOTHING ELSE.
     *
     * Before the soundfont, because loading a 148 MB font to answer
     * "how big would this be" would be absurd - a MIDI parse is
     * milliseconds and is all this needs.
     *
     * WHAT THE NUMBER IS MADE OF, and only two settings move it:
     *
     *   the tempo map      smf_load integrates it into length_usec
     *   + releases         voices still sounding at the last event
     *   + 2 s if -E on     the effects tail (see `tail' below)
     *   clamped to the cap RENDER_CAP_BYTES at this rate (it was a flat
     *                      1200 s until 2026-10-04)
     *   x rate x 4         16-bit stereo
     *
     * VOICES, GAIN, THE VOLUME LAW AND THE FILTER CHANGE NOTHING.
     * `-p 2048' is slower, not bigger, and a user told otherwise would
     * turn it down for the wrong reason.
     *
     * THE RELEASE ALLOWANCE IS THE ONLY GUESS. It depends on which
     * voices are sounding when the music stops and on their envelopes,
     * neither of which is knowable without rendering. Measured on
     * gmstriving.mid 2026-09-22: 172.1 s of events produced 174.1 s of
     * audio with effects OFF, so 2.0 s of pure release. Three is used
     * here because the caller's question is "will this fit", and an
     * over-estimate costs a warning the user can dismiss while an
     * under-estimate costs the failure this exists to prevent.
     *
     * SO IT IS PRINTED AS AN ESTIMATE AND THE CALLER MUST SAY SO.
     */
    if (estimate) {
        unsigned long secs, bytes;

        mfp = fopen(argv[2], "rb");
        if (mfp == NULL) { perror(argv[2]); return 1; }
        smf_set_memory_limit(midi_memory_limit());
        if (smf_load(&smf, mfp) < 0) {
            fprintf(stderr, "smf2wav: %s: %s\n", argv[2], smf_error());
            return 1;
        }
        fclose(mfp);

        {
            int capped;

            secs = render_need_sec(&smf, effects);   /* + releases, tail */
            capped = secs > render_cap_sec(rate);
            if (capped)
                secs = render_cap_sec(rate);

            bytes = secs * (unsigned long) rate * 4UL;

            /* ON stdout AND PARSEABLE, one value per line, because the
             * GUI reads this and a human may also run it. `capped' is
             * new (2026-10-04) and a reader that does not know it skips
             * it - the GUI reads its keys one by one. */
            printf("estimate_seconds %lu\n", secs);
            printf("estimate_bytes %lu\n", bytes);
            printf("rate %d\n", rate);
            if (capped)
                printf("capped 1\n");
        }
        return 0;
    }

    sfp = fopen(argv[1], "rb");
    if (sfp == NULL) { perror(argv[1]); return 1; }
    if (sf2_load(&sf, sfp) < 0) {
        fprintf(stderr, "smf2wav: %s: %s\n", argv[1], sf2_error());
        return 1;
    }

    if (!quiet) {
        int n;
        fprintf(stderr, "soundfont: %s (%d presets, %d samples)\n",
                sf.name, sf.npresets, sf.nsamples);
        n = sf2_zone_warn(&sf, warn_cb, NULL);
        if (n > 0)
            fprintf(stderr, "  %d generator types present that this"
                            " renderer does not act on\n", n);
    }

    pool = load_pool(sfp, &sf, &pool_frames);
    fclose(sfp);
    if (pool == NULL) {
        fprintf(stderr, "smf2wav: cannot load the sample pool\n");
        return 1;
    }

    mfp = fopen(argv[2], "rb");
    if (mfp == NULL) { perror(argv[2]); return 1; }
    smf_set_memory_limit(midi_memory_limit());
    if (smf_load(&smf, mfp) < 0) {
        fprintf(stderr, "smf2wav: %s: %s\n", argv[2], smf_error());
        return 1;
    }
    fclose(mfp);

    /* EVERY -s ARGUMENT CHECKED BEFORE THE OUTPUT EXISTS - 2026-10-05.
     * A bad @BANK was refused only after OUT.wav had been created with
     * its header, so a typo left a 44-byte WAV behind. The fonts are
     * still loaded below; this only refuses a malformed argument first. */
    for (i = 0; i < nx; i++) {
        char  vname[512];
        int   vbank = 0;

        if (font_arg(xname[i], vname, sizeof vname, &vbank) < 0) {
            fprintf(stderr, "smf2wav: -s %s: FILE[@BANK], BANK 0..127\n",
                    xname[i]);
            return SMF2WAV_EXIT_USAGE;
        }
    }

    out = fopen(argv[3], "wb");
    if (out == NULL) { perror(argv[3]); return 1; }
    wav_header(out, rate, 0);

    render_init(&r, &sf, pool, pool_frames, rate);
    if (verbose)
        r.log = stderr;

    /*
     * THE CEILING. RENDER_MAX_VOICES (2048) is this tool's bound and
     * SMF2WAV_DEFAULT_VOICES (512) its default - offline rendering has
     * no deadline (measured 2026-09-19: 2048 renders gmstriving no
     * slower than 512).
     *
     * AND THE MEMORY NOW FOLLOWS IT - design/21 section 17, built
     * 2026-10-02. The array is allocated to the ceiling asked for, so
     * -p 64 costs 64 voices where it used to cost the whole 2048;
     * -p auto reserves the bound, since it may grow into all of it.
     */
    {
        int want = grow ? 1 : (want_voices > 0 ? want_voices
                                               : SMF2WAV_DEFAULT_VOICES);

        /* Start LOW with -p auto and let it climb, so grew_to reports
         * the true demand rather than "it never needed more than the
         * start". */
        if (render_set_max_voices(&r, want) != want
            || r.voices == NULL) {
            fprintf(stderr, "smf2wav: no memory for %d voices\n", want);
            return 1;
        }
        if (grow)
            render_set_grow(&r, 1);
    }
    for (i = 0; i < nx; i++) {
        char  name[512];
        int   bank = 0;
        long  xframes;
        short *xpool;
        FILE *xfp;

        if (font_arg(xname[i], name, sizeof name, &bank) < 0) {
            fprintf(stderr, "smf2wav: -s %s: FILE[@BANK], BANK 0..127\n", xname[i]);
            return SMF2WAV_EXIT_USAGE;
        }
        xfp = fopen(name, "rb");
        if (xfp == NULL) { perror(name); return 1; }
        if (sf2_load(&xsf[i], xfp) < 0) {
            fprintf(stderr, "smf2wav: %s: %s\n", name, sf2_error());
            return 1;
        }
        xpool = load_pool(xfp, &xsf[i], &xframes);
        fclose(xfp);
        if (xpool == NULL) {
            fprintf(stderr, "smf2wav: %s: cannot load the sample pool\n", name);
            return 1;
        }
        render_add_font(&r, &xsf[i], xpool, xframes, bank);
        if (!quiet)
            fprintf(stderr, "soundfont: %s (%d presets, %d samples) at bank %d\n",
                    xsf[i].name, xsf[i].npresets, xsf[i].nsamples, bank);
    }
    render_set_velfilter(&r, velfilter | law);
    render_set_fx(&r, effects);
    render_set_filter_bypass(&r, bypass);
    render_set_menv_mode(&r, menv);
    if (master > 0)
        render_set_master(&r, master);
    if (!quiet) {
        /* SPLIT, for the reason in vmidid.c: -F's value beside -L's
         * read as one setting, and AWE names three things between
         * them. BRACED since 2026-10-05: only the first line was under
         * the `if', so -q still printed the other two. */
        fprintf(stderr, "velocity filter: %s (-F)\n",
                sf2_mod_vf_name(velfilter));
        fprintf(stderr, "volume curve: %s\n", sf2_mod_law_desc(law));
        fprintf(stderr, "effects %s, bypass %s, master %.3f,"
                        " modulation envelope %s\n",
                render_fx_name(effects), bypass ? "on" : "off",
                r.master / 1024.0,
                menv == RENDER_MENV_FAST ? "fast" : "reference");
    }

    if (verbose) {
        trace_rate    = (long) rate;
        r.trace       = trace_voice;
        r.trace_arg   = &trace_rate;
        fprintf(stderr, "  voice trace: times in ms, S is the sustain"
                        " gain, pan is right-left\n");
    }

    /*
     * THE TAIL - FOR THE EFFECTS, NOT THE RELEASES. A voice in release
     * still counts as active, so the loop below already runs until
     * the last release has finished; this allowance is what runs on
     * after that, while the reverb (and the chorus line) decay. Two
     * seconds is longer than the reverb's tail at any pitch - measured
     * 2026-09-08, 0.9 s to silence on a bright tone, 1.4 s on a dull
     * one (design/21 section 13), and it decays to exact zero. With
     * -E off nothing follows the last voice and the render ends there
     * as it always did, so an effects-off WAV stays bit-identical to
     * every earlier build's.
     *
     * HISTORY: this variable was computed and then discarded with a
     * `(void) tail' for two commits, because releases turned out to be
     * covered by render_active() and nothing else needed it. The
     * effects did: the first isolated-note listen test (set 18) ended
     * 0.3 s after its last note-off with the reverb at +10 dB.
     */
    tail = (unsigned long) rate * 2UL;
    ceiling_sec = render_ceiling_sec(&smf, rate, effects);

    /* WHAT THIS RENDER WILL BE, SAID AT THE START - 2026-10-04. Run by
     * hand, a long file is seen before the minutes pass; nothing asks,
     * so a script is not stopped. stderr, with the other diagnostics. */
    if (!quiet) {
        unsigned long need = render_need_sec(&smf, effects);

        if (need > ceiling_sec)
            need = ceiling_sec;
        fprintf(stderr, "smf2wav: about %lu:%02lu of audio, about %lu MB%s\n",
                need / 60UL, need % 60UL,
                (need * (unsigned long) rate * 4UL + 524288UL) / 1048576UL,
                need < render_need_sec(&smf, effects)
                    ? " - longer than the cap, so it stops there" : "");
    }

    /*
     * TIME IS CARRIED IN MICROSECONDS, NOT DERIVED FROM THE FRAME
     * COUNT.
     *
     * `(frames + BLOCK) * 1000000UL / rate' overflows an unsigned
     * long at 4294 frames - 97 MILLISECONDS of audio - after which
     * the deadline wraps and no further event is ever delivered. It
     * rendered 4 notes of a 1482-note file and then ran to the
     * 20-minute ceiling with an empty mixer.
     *
     * C89 has no guaranteed 64-bit type, so the fix is not a wider
     * multiply: accumulate the per-block duration instead, which
     * never exceeds a few million.
     */
    {
    /*
     * EACH BLOCK'S TIMES COMPUTED FROM ITS NUMBER, NOT ADDED UP - 2026-10-04.
     * This was `now_usec += block_usec', with block_usec rounded down
     * (5804 for 5804.98 at 44100), so the song clock ran 0.017% slow and
     * every event landed later than the last: 0.26 s by the end of a
     * 25-minute file. Doubles, as smf.h's times are; one divide per
     * block, none per event or per sample.
     */
    unsigned long blk = 0;              /* blocks begun */
    double        now_usec = 0.0;       /* the end of the current block */

    for (;;) {
        double block_start_usec, block_end_usec;
        int n;

        block_start_usec = now_usec;
        blk++;
        block_end_usec = (double) blk * (double) BLOCK * 1000000.0
                         / (double) rate;
        now_usec = block_end_usec;

        /*
         * RENDER UP TO EACH EVENT, NOT A WHOLE BLOCK AT A TIME.
         *
         * Pumping every event due within the block and THEN mixing it
         * loses any note shorter than one block: the note-on and its
         * note-off both arrive before a single frame is rendered, so
         * the voice is started and stopped having produced nothing.
         *
         * jazz.mid opens with four side-stick hits (channel 9, key
         * 37) of 16 ms each against a 23 ms block. All four vanished.
         * A listener who knows the piece spotted it immediately;
         * every counter in this harness said 4064 notes started, all
         * mapped, none stolen - because they HAD started. They just
         * never sounded.
         *
         * So the block is split at event boundaries. `done' tracks
         * how far into the block we have rendered; each pass mixes up
         * to the next event's time, applies it, and continues.
         */
        {
            int done = 0;

            while (done < BLOCK) {
                int upto = BLOCK;

                if (ev < smf.nevents &&
                    smf.events[ev].usec <= block_end_usec) {
                    /* Where in this block does the event fall? Under
                     * one block from its start, so this stays small. */
                    double into = smf.events[ev].usec > block_start_usec
                                ? smf.events[ev].usec - block_start_usec
                                : 0.0;
                    upto = (int) (into * (double) BLOCK
                                  / (block_end_usec - block_start_usec));
                    if (upto > BLOCK) upto = BLOCK;
                    if (upto < done)  upto = done;
                }

                if (upto > done) {
                    render_mix(&r, block + done * 2, upto - done);
                    done = upto;
                }

                if (ev < smf.nevents &&
                    smf.events[ev].usec <= block_end_usec) {
                    if (smf.events[ev].status == 0xf0)
                        render_sysex(&r, smf.sysex + smf.events[ev].sx_off,
                                     smf.events[ev].sx_len);
                    else
                        render_event(&r, smf.events[ev].status,
                                     smf.events[ev].data1,
                                     smf.events[ev].data2);
                    ev++;
                } else if (done >= BLOCK) {
                    break;
                } else {
                    render_mix(&r, block + done * 2, BLOCK - done);
                    done = BLOCK;
                }
            }
        }
        n = render_active(&r);
        if (n > maxvoices) maxvoices = n;

        for (i = 0; i < BLOCK * 2; i++) {
            int a = block[i] < 0 ? -block[i] : block[i];
            if ((unsigned long) a > peak) peak = (unsigned long) a;
        }

        wr(block, 2, BLOCK * 2, out);
        if (g_wr_failed)
            break;              /* the disk is full or gone */
        frames += BLOCK;

        /*
         * PROGRESS, ONCE A SECOND OF OUTPUT AUDIO - not once a second
         * of wall clock, which would need a timer this program does
         * not have. On a machine slower than real time the line moves
         * slower than a second; on a fast one, faster. Either way it
         * moves, which is what the caller needs.
         *
         * ON stdout, NOT stderr. The GUI reads this pipe and stderr
         * carries the diagnostics - keeping them apart means a warning
         * cannot be parsed as a percentage. `-q' silences it with
         * everything else.
         *
         * FLUSHED EVERY TIME. The reader is a pipe, so stdio would
         * otherwise buffer a whole block of these and deliver them in
         * bursts - which is exactly the progress bar that jumps from
         * 0 to 100.
         */
        if (!quiet && frames % (unsigned long) rate < BLOCK) {
            printf("progress %lu\n", frames);
            fflush(stdout);
        }

        if (ev >= smf.nevents) {
            if (n == 0) {
                /* Every voice has ended. Run on through the effects'
                 * tail, then stop; without effects stop at once. */
                if (!effects || after >= tail)
                    break;
                after += BLOCK;
            } else if (now_usec > smf.length_usec + 2000000.0) {
                break;                  /* a stuck voice must not hang us */
            }
        }
        if (frames > (unsigned long) rate * ceiling_sec) {
            /* SAY SO. Silent until 2026-09-22, which made a cut
             * render indistinguishable from a complete one. */
            fprintf(stderr, "smf2wav: stopped at the ceiling, %lu:%02lu"
                            " - the output is INCOMPLETE\n",
                    ceiling_sec / 60UL, ceiling_sec % 60UL);
            ceiling_hit = 1;
            break;
        }
    }
    }
    /*
     * THE HEADER IS WRITTEN FROM WHAT REACHED THE DISK, and only when
     * everything did. On a failed write the old code rewound and
     * wrote the frame count it INTENDED, so a half-written file
     * claimed its full duration - a file that lies about itself,
     * which is worse than a visibly short one.
     */
    if (ferror(out))
        g_wr_failed = 1;

    if (!g_wr_failed) {
        rewind(out);
        wav_header(out, rate, frames);
    }

    /* fclose IS A WRITE. On a full disk it is often the ONLY call
     * that can report the failure, because everything before it sat
     * in stdio's buffer. */
    if (fclose(out) != 0)
        g_wr_failed = 1;

    if (g_wr_failed) {
        fprintf(stderr, "smf2wav: %s: write failed - %s\n",
                argv[3], strerror(errno));
        fprintf(stderr, "  the output is INCOMPLETE and its header"
                        " has been left saying so\n");
        return 1;
    }

    /* AT THE CEILING THE EXIT IS 2, WITH OR WITHOUT -q - design/54 D18,
     * 2026-10-04. It was 2 with -q and 0 without, and the GUI never
     * passes -q: its "file is incomplete" message for exit 2 could not
     * fire, and a cut render was reported as finished. Without -q the
     * statistics below are still printed first. */
    if (ceiling_hit && quiet)
        return 2;               /* -q: the stderr line above is all */

    if (!quiet) {
        fprintf(stderr, "rendered %lu frames (%.1f s) at %d Hz\n",
                frames, (double) frames / rate, rate);
        fprintf(stderr, "  notes started %lu, stolen %lu, unmapped %lu,"
                        " clipped %lu\n",
                r.notes_started, r.notes_stolen, r.notes_unmapped,
                r.clipped);
        /* AGAINST THE CEILING IN USE, not the array size. Reporting
         * "409 of 2048" when -p said 512 describes the build rather
         * than the render, and hides the case that matters: a peak
         * EQUAL to the ceiling means voices were being stolen. */
        fprintf(stderr, "  peak voices %d of %d, peak sample %lu of 32767\n",
                maxvoices, r.max_voices, peak);
        if (r.notes_unmapped > 0)
            fprintf(stderr, "  WARNING: %lu notes had no zone - wrong bank"
                            " or a preset the soundfont lacks\n",
                    r.notes_unmapped);

        /*
         * SAY WHAT THE STEAL COUNT MEANS. `stolen 980' is a fact
         * nobody can act on without already knowing what it implies;
         * the ceiling that would have avoided it is the useful half,
         * and this tool is the one place that can say it.
         */
        if (r.notes_stolen > 0 && !r.grow)
            fprintf(stderr, "  NOTE: %lu voices were stolen at -p %d."
                            " Re-run with `-p auto' to find what this\n"
                            "        file actually needs, or a higher"
                            " -p to avoid it.\n",
                    r.notes_stolen, r.max_voices);

        if (r.grew_to > 0)
            fprintf(stderr, "  this file needs %d voices"
                            " (grown from 1 by -p auto)\n", r.grew_to);
    }

    render_free(&r);
    smf_free(&smf);
    sf2_free(&sf);
    free(pool);
    return ceiling_hit ? 2 : 0;
}
