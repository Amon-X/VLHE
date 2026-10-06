/*
 * vsoundd.c - the pump.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/07-vsound.md sections 3 and 4. It does ONE thing: move mixed
 * audio from vsound to the card, and report what the card did.
 *
 *     vsoundd [-d card] [-v vsound] [-o file] [-R] [-S rate] [-q]
 *
 * NO DECISIONS ARE MADE HERE. No mixing, no conversion, no buffering
 * policy, no per-fragment metadata. Every one of those lives in the
 * kernel now, which is the whole point of the rewrite: the old tree's
 * daemon made decisions, and a fault could be in either half with no
 * way to tell which.
 *
 * It exists at all only because a kernel module cannot reach esssolo1 -
 * that driver registers its own fops and never enters audio_devs[], so
 * audio_open() cannot find it (section 2). If the card were reachable
 * from the kernel this file would not exist.
 *
 * C89, and built by GCC 2.95.2 for the target.
 */

/*
 * POSIX, REQUESTED EXPLICITLY - the same reason vmidicat.c and
 * vmidid.c do it: sigaction and struct sigaction live behind
 * __USE_POSIX in this libc's signal.h, which `gcc -ansi' does not
 * define on its own. Without it check-gcc295 reports them as implicit
 * declarations.
 */
#define _POSIX_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>           /* stat - the card is not vsound itself */
#include <sys/time.h>           /* gettimeofday, for the write timing */
#include <sys/soundcard.h>

#include "vsound.h"
#include "vlhe_status.h"

/* THE RATES vsound CAN MIX AT - VSOUND_RATE_MIN/MAX in the module's
 * modules/vsound/vsound_chan.h, which VSOUND_IOC_MIXRATE enforces with
 * -EINVAL. Copied rather than included: that header is the module's
 * private one. Keep the two in step. */
#define MIX_RATE_MIN    4000
#define MIX_RATE_MAX    48000

#define VSOUND_DEV  "/dev/dsp"
#define CARD_DEV    "/dev/dsp1"
#define RATE        44100
#define CHANNELS    2
#define CHUNK       4096

static int running = 1;

static void
on_signal(int sig)
{
    (void) sig;
    running = 0;
}

static void
usage(void)
{
    fprintf(stderr,
        "usage: vsoundd [-d card] [-v vsound] [-o file] [-R] [-S rate] [-q]\n"
        "  -d DEV   the real card (default %s)\n"
        "  -v DEV   vsound's device (default %s)\n"
        "  -o FILE  ALSO write everything to FILE, raw, at the MIX rate\n"
        "           (%d Hz unless -S or the card set another), %d ch\n"
        "           16-bit LE - aplay -f S16_LE -r RATE -c %d FILE\n"
        "  -R       release the card when the last client closes, so the\n"
        "           driver's own teardown runs. Fixes a note that hangs\n"
        "           after the last client on esssolo1, es1370, es1371,\n"
        "           cmpci and sonicvibes. Off by default: on any other\n"
        "           card it only adds a reacquisition risk.\n"
        "  -S RATE  mix at RATE rather than %d, 4000 to 48000.\n"
        "           THE CARD STILL WINS:\n"
        "           a rate it cannot do is clamped by the driver and\n"
        "           the clamped value is what gets used. Useful where\n"
        "           the card's own answer cannot be trusted - an\n"
        "           emu10k1 on Corel's packaged driver echoes the\n"
        "           request back unchanged, and its DAC is 48000.\n"
        "  -q       quiet\n",
        CARD_DEV, VSOUND_DEV, RATE, CHANNELS, CHANNELS, RATE);
}

/*
 * Open the card and negotiate.
 *
 * ORDER MATTERS and it is not obvious: OSS allocates the buffer on the
 * first write or the first SPEED, and SETFRAGMENT is ignored once that
 * has happened - silently, with no error. So geometry first, format
 * after.
 *
 * O_NONBLOCK on the open, never on the writes: esssolo1's OPEN blocks
 * rather than returning EBUSY when the device is taken, which disguised
 * device contention for days on the old tree (CLAUDE.md section 5). The
 * write is allowed to block - that is the pacing.
 */
/* vsound's own device number, once it is open - see open_card(). */
static dev_t g_vs_rdev;
static int   g_vs_rdev_known;

static int
open_card(const char *dev, int *frag_out, int quiet, int want_rate,
          int *rate_out, int say_error)
{
    audio_buf_info bi;
    struct stat st;
    int fd, v;

    /*
     * NOT VSOUND ITSELF - design/54 D04, 2026-10-04 (design/36 row 66).
     * The plan refuses a card that is vsound, but the daemon trusted its
     * -d: given vsound's own node - or /dev/dsp after it had become
     * vsound's link, which matters on the reclaim path, where the card
     * is reopened by name long after startup - the pump would read its
     * own output and write it straight back in. stat() follows the link,
     * so a name and a link to the same node compare equal. ENXIO, which
     * vlhe_card_open_retryable() treats as final: waiting cannot fix it.
     */
    if (g_vs_rdev_known && stat(dev, &st) == 0 && S_ISCHR(st.st_mode)
        && st.st_rdev == g_vs_rdev) {
        fprintf(stderr, "vsoundd: %s is vsound's own device, not a card -"
                        " the pump would feed itself; not opening it\n",
                dev);
        errno = ENXIO;
        return -1;
    }

    fd = open(dev, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        /* errno IS KEPT ACROSS THE MESSAGE - callers act on it, and
         * stdio may set it. `say_error' is 0 only in wait_for_card(),
         * which says it once rather than ten times a second. */
        int e = errno;

        if (say_error)
            fprintf(stderr, "vsoundd: %s: %s\n", dev, strerror(e));
        errno = e;
        return -1;
    }
    /* Clear O_NONBLOCK now the open has succeeded: from here the write
     * blocking IS the flow control. */
    fcntl(fd, F_SETFL, 0);

    v = AFMT_S16_LE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &v) < 0 || v != AFMT_S16_LE)
        fprintf(stderr, "vsoundd: %s: format is 0x%x, wanted S16_LE\n",
                dev, v);

    v = CHANNELS;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) < 0 || v != CHANNELS)
        fprintf(stderr, "vsoundd: %s: %d channels, wanted %d\n",
                dev, v, CHANNELS);

    /*
     * THE RATE THE CARD ACTUALLY GRANTS, REPORTED TO THE CALLER.
     *
     * This used to print a warning and throw `v' away, and the module
     * mixed at the compile-time constant regardless - so on a card
     * that cannot do 44100 everything played at the wrong pitch and
     * speed with no error anywhere. NO 2.2 DRIVER RESAMPLES: they
     * clamp to what the hardware can do and return the truth (SB Pro
     * caps at 22050 IN STEREO, SB 1.0 at 23000, esssolo1 picks the
     * nearer crystal divisor). design/25 section 7b has the survey.
     */
    v = RATE;
    if (want_rate > 0)
        v = want_rate;          /* the user's override, still clamped
                                 * by the card below */
    if (ioctl(fd, SNDCTL_DSP_SPEED, &v) < 0) {
        fprintf(stderr, "vsoundd: %s: SPEED failed\n", dev);
        v = RATE;               /* nothing better to believe */
    }
    if (rate_out != NULL)
        *rate_out = v;

    /* ASK, never assume - the card has the final say and the number is
     * only meaningful read back. */
    *frag_out = 0;
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi) == 0 && bi.fragsize > 0) {
        *frag_out = bi.fragsize;
        /*
         * `v', NOT `RATE' - corrected 2026-09-26, design/36 row 89.
         *
         * This printed the COMPILE-TIME CONSTANT in both places: the
         * rate it names and the divisor the duration is computed
         * from. The whole point of the SPEED ioctl above is that `v'
         * comes back as what the card GRANTED, which may not be what
         * was asked - and this line, the first thing a user reads,
         * claimed 44100 regardless.
         *
         * BOTH HALVES WERE WRONG AND THE SECOND IS WORSE. On a card
         * clamped to 22050 it would say "at 44100 Hz", and the
         * millisecond figure would be HALF the truth, because the
         * buffer holds twice as long at half the rate. A wrong
         * latency reading is what someone tunes fragments against.
         *
         * IT WAS INVISIBLE HERE BECAUSE THE es1371 GRANTS 44100, so
         * constant and truth agreed on every machine this project
         * tests on. The `mixing at %d Hz' line two steps later uses
         * the negotiated value and always did.
         */
        if (!quiet)
            fprintf(stderr, "vsoundd: %s: %d x %d = %ld ms at %d Hz\n",
                    dev, bi.fragstotal, bi.fragsize,
                    (long) bi.fragstotal * bi.fragsize * 1000L
                    / ((long) v * 4L), v);
    }
    return fd;
}

/*
 * A BUSY CARD IS WAITED FOR, NOT GIVEN UP ON - design/54 section 6b,
 * 2026-10-03. vsoundd used to exit on any failed card open, so a pump
 * started while vmidid or vdiscd still played straight to that card
 * (`@VSOUND:<card>@', Sound not in their load) failed its step. Now it
 * says WAITING in its state file - a daemon on that very card lets go
 * when it sees it - and retries every CARD_POLL_MS for CARD_WAIT_MS.
 *
 * WHICH FAILURES ARE RETRIED IS THE CARD SURVEY (design/54 6c): every
 * errno but ENOENT and ENXIO, because the drivers do not agree on one
 * for "busy" - EBUSY, EWOULDBLOCK, and ENODEV from i810 and trident.
 * An emu10k1 never comes here: it is not exclusive for playback, so
 * the first open succeeds beside the daemon.
 *
 * Returns the fd, or -1 with errno from the last attempt. A signal
 * ends the wait - `running' is cleared and the select() returns early.
 */
#define CARD_WAIT_MS    10000
#define CARD_POLL_MS    100

static int
wait_for_card(const char *dev, int *frag_out, int quiet, int want_rate,
              int *rate_out)
{
    int  fd, err = errno;
    long waited = 0;

    fprintf(stderr, "vsoundd: %s is busy (%s) - waiting up to %d s"
                    " for it\n", dev, strerror(err), CARD_WAIT_MS / 1000);
    (void) vlhe_pump_state_write(dev, VLHE_PUMP_WAITING);

    while (running && waited < CARD_WAIT_MS) {
        /* select() AS THE SLEEP, not usleep(): this file defines
         * _POSIX_SOURCE, which hides usleep() in the target's
         * unistd.h, and 2.95.2 warned on the implicit declaration
         * (the check-gcc295 gate, design/54 D08). select() is declared
         * unconditionally (sys/select.h) and a signal still ends it. */
        struct timeval tv;

        tv.tv_sec  = 0;
        tv.tv_usec = CARD_POLL_MS * 1000L;
        (void) select(0, (fd_set *) 0, (fd_set *) 0, (fd_set *) 0, &tv);
        waited += CARD_POLL_MS;
        fd = open_card(dev, frag_out, quiet, want_rate, rate_out, 0);
        if (fd >= 0) {
            fprintf(stderr, "vsoundd: got %s after %ld ms\n", dev, waited);
            return fd;
        }
        err = errno;
        if (!vlhe_card_open_retryable(err))
            break;
    }
    fprintf(stderr, "vsoundd: %s: still %s after %ld ms - giving up\n",
            dev, strerror(err), waited);
    if (err == ENODEV)
        fprintf(stderr, "vsoundd: ENODEV is either no driver behind %s,"
                        " or an i810/trident with every channel in"
                        " use\n", dev);
    errno = err;
    return -1;
}

int
main(int argc, char **argv)
{
    const char *carddev = CARD_DEV;
    const char *vsdev   = VSOUND_DEV;
    const char *outfile = NULL;
    unsigned char buf[CHUNK];
    struct vsound_ack ack;
    count_info cinfo;
    unsigned long written = 0;
    /*
     * PUMP TIMING, added 2026-08-31. See the write loop for why.
     *
     * PUMP_SLOW_MS: the pump hands over one hard-buffer read at a
     * time, which at 44100 stereo is tens of milliseconds of audio, so
     * a write blocking 100 ms means the card was already full and this
     * process sat waiting - the pacing working as intended if it is
     * occasional, and the thing to explain if it is not.
     *
     * PUMP_REPORT: one summary line per N writes. At ~100 writes a
     * second, 500 is roughly one line every five seconds, which
     * matches the audio child's LOG_FRAMES so the two logs can be read
     * side by side.
     */
    unsigned long pump_calls = 0, pump_slow = 0, pump_worst = 0;
    int pump_said = 0;
#define PUMP_SLOW_MS    100
#define PUMP_REPORT     500
    int cardfd, vsfd, outfd = -1, frag = 0, quiet = 0;
    /*
     * THE MIX RATE. `want_rate' is the user's -S, 0 for "no opinion";
     * `card_rate' is what the card actually granted, which is what
     * the module is told to mix at. They differ whenever the hardware
     * cannot do what was asked - see open_card().
     */
    int want_rate = 0, card_rate = 0;
    int i, n, w, odelay;
    /*
     * -R: RELEASE THE CARD WHEN THE LAST CLIENT GOES.
     * design/16-hanging-note.md section 4, Design B. Default OFF: it
     * costs a reacquisition risk that only an affected card repays.
     */
    int release = 0, said_lost = 0, reclaim = 0, had_client = 0;
    int reclaim_errno = 0;   /* errno of the last failed reacquire */
    unsigned long releases = 0, reacquires = 0;
    /* Ticks of failed reacquisition before saying so. At ~100 a
     * second these are a few hundred ms, which covers the ~100 ms
     * drain window (design/16 section 4) with room to spare. */
#define RECLAIM_TICKS   30
    struct vsound_stat st;
    struct sigaction sa;
    /*
     * LINE BUFFERING, AND IT IS NOT COSMETIC.
     *
     * load.sh redirects this daemon's stdout to DAEMON.LOG, and libc
     * block-buffers a FILE - so everything printed sits in a 4 KB
     * buffer until the process exits. On 86Box 2026-09-19 that meant
     * DAEMON.LOG held vsoundd and vmidid output and NOTHING from
     * vdiscd: it prints a handful of lines and never fills a block,
     * so its attach banner, its swap messages and the control
     * channel's own logging were all invisible while the daemon ran.
     *
     * THE OTHER TWO ONLY LOOK FINE BY ACCIDENT - vsoundd prints a
     * progress line every 500 writes and so crosses 4 KB early.
     * Neither flushed either.
     *
     * A LOG YOU CANNOT READ UNTIL THE PROGRAM EXITS IS NOT A LOG for
     * a daemon, and it is worse than useless when diagnosing a hang:
     * the evidence is in the address space of the process that will
     * not exit.
     */
    setvbuf(stdout, (char *) 0, _IOLBF, 0);

    /* THE OPTIONS FIRST, then the pid file - 2026-10-05. A mistyped
     * option printed the usage and exited AFTER the pid file was
     * written, leaving the Status page a "stale pid file". Reading the
     * arguments touches nothing, so nothing is lost by doing it first. */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)      carddev = argv[++i];
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) vsdev   = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "-q"))                 quiet   = 1;
        else if (!strcmp(argv[i], "-R"))                 release = 1;
        /* -S: the MIX RATE the user wants. Still clamped by the card,
         * because the hardware always wins - an override that were
         * silently impossible would be worse than none. */
        /*
         * RANGE-CHECKED SINCE 2026-10-05 (the G01 documentation
         * review). It was a bare atoi(): "-S abc" silently meant no
         * rate, and "-S 96000" went to the card - which an emu10k1 on
         * Corel's packaged driver ECHOES back, so vsound refused it
         * and kept mixing at 44100 while the card played the data as
         * 96000, twice the speed. The config path was always safe
         * (vlhe_mix_rate() passes 8000-48000 only); this is for a
         * hand-typed -S.
         */
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) {
            char *end;
            long r = strtol(argv[++i], &end, 10);

            if (end == argv[i] || *end != '\0'
                || r < MIX_RATE_MIN || r > MIX_RATE_MAX) {
                fprintf(stderr, "vsoundd: -S %s: the mix rate must be"
                                " %d to %d Hz\n", argv[i],
                        MIX_RATE_MIN, MIX_RATE_MAX);
                return 1;
            }
            want_rate = (int) r;
        }
        else { usage(); return 1; }
    }

    /* THE PID FILE, so the Status page can tell running from
     * stopped. Until 2026-09-19 no daemon wrote one and the page
     * said "no pid file" for all three while all three ran. */
    /*
     * AND IT CAN REFUSE - the return is no longer ignored.
     *
     * -2 MEANS ONE IS ALREADY RUNNING. Starting a second would take
     * the device the first holds, fail, exit, and leave the pidfile
     * naming a dead process - which is exactly what happened on
     * 86Box 2026-09-23 when the user pressed Load twice: the orphaned
     * first daemon then kept the module loaded through an Unload that
     * reported success.
     *
     * A WRITE FAILURE IS NOT FATAL. The pidfile is how the Status
     * page and the teardown FIND us; a daemon that cannot write one
     * still works, and refusing to start over it would be worse than
     * being hard to stop.
     */
    {
        int prc = vlhe_status_write_pidfile("vsoundd");

        if (prc == -2) {
            fprintf(stderr, "vsoundd: already running - not starting a"
                            " second one\n");
            return 1;
        }
        if (prc != 0)
            fprintf(stderr, "vsoundd: warning: could not write the pid"
                            " file; the Status page will not see"
                            " this daemon\n");
    }
    /* AND THE STATE FILE GOES WITH US, on every return from here - the
     * daemons read it to decide whether to move (design/54 6b). Only
     * after the pid file, so a second instance refused above cannot
     * remove the first one's. */
    atexit(vlhe_pump_state_remove);



    /*
     * sigaction WITH sa_flags = 0, NOT signal() - fixed 2026-09-14.
     *
     * signal() on glibc implies SA_RESTART, so SIGTERM ran on_signal,
     * `running' went to 0, and the kernel then restarted the read()
     * this loop blocks in. The flag is only tested at the top of the
     * loop, which is reached only when a read RETURNS - so a pump
     * with audio flowing stopped on the next chunk and an IDLE pump
     * never stopped at all. Every hand-started pump killed on the
     * Acer on 2026-09-14 needed -9 for this reason
     * (tests/logs/2026-09-14-acer-71-...), and unload.sh's escalation
     * to -9 after a second hid it for every scripted teardown, at the
     * cost of the "stopping" summary. vdiscd, vmidicat and vmidid had
     * all already made this change; vmidicat.c's comment has the
     * sequence. With no SA_RESTART the read returns EINTR, the
     * `continue' below tops the loop, and `running' is seen.
     */
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /*
     * O_RDONLY: we DRAIN the mixed stream, we do not produce audio.
     *
     * The module has to allow that, and for one run it did not - it
     * refused every open without FMODE_WRITE with "playback only",
     * which was correct before read() existed and wrong the moment it
     * did. The error was -EINVAL, which reads as "bad argument" and
     * says nothing about the mode, so the message below names the mode
     * explicitly.
     */
    vsfd = open(vsdev, O_RDONLY);
    if (vsfd < 0) {
        fprintf(stderr, "vsoundd: %s (O_RDONLY): %s\n",
                vsdev, strerror(errno));
        if (errno == EINVAL)
            fprintf(stderr, "vsoundd: the module refused a read-only"
                            " open - is it a version that supports the"
                            " pump?\n");
        return 1;
    }
    {
        struct stat vst;

        if (fstat(vsfd, &vst) == 0 && S_ISCHR(vst.st_mode)) {
            g_vs_rdev = vst.st_rdev;
            g_vs_rdev_known = 1;
        }
    }

    cardfd = open_card(carddev, &frag, quiet, want_rate, &card_rate, 1);
    if (cardfd < 0 && vlhe_card_open_retryable(errno))
        cardfd = wait_for_card(carddev, &frag, quiet, want_rate,
                               &card_rate);
    if (cardfd < 0) {
        /* The most common cause by far, and worth naming rather than
         * leaving as errno: a /dev/dspN NODE exists whether or not a
         * driver is registered behind it, so the open fails with
         * ENODEV rather than ENOENT.
         *
         * CORRECTED 2026-10-05 (the G01 documentation review): this
         * said "no driver holds audio device 1", told the user to
         * check /proc/sound and to "Load sb AFTER vsound". All three
         * were stale - the card is whatever -d names, PCI cards never
         * appear in /proc/sound, and VLHE loads no card driver at all,
         * only vsound. */
        if (errno == ENODEV || errno == ENXIO || errno == ENOENT)
            fprintf(stderr, "vsoundd: %s: no sound card driver is loaded"
                            " behind it.\n", carddev);

        /*
         * WITHOUT A CARD, STILL CAPTURE. -o is a diagnostic that must
         * not depend on the thing being diagnosed: the whole point is
         * to check what the MIXER produced when the card path is
         * suspect, and the card path being absent is the strongest
         * possible version of suspect.
         *
         * The first version exited here, so a run with -o and no card
         * produced no file at all - the one case where the capture was
         * most wanted.
         */
        if (outfile == NULL) {
            close(vsfd);
            return 1;
        }
        fprintf(stderr, "vsoundd: no card - capturing to %s ONLY\n",
                outfile);
    }

    /*
     * TELL THE MODULE WHAT THE CARD ACTUALLY GRANTED.
     *
     * NOW, and not earlier: the card has been negotiated with, and no
     * client can be running yet - the module refuses the change once
     * any channel is open, because each one computes its resampling
     * ratio from the mix rate at the moment its format is set.
     *
     * An older module without this ioctl returns -EINVAL, which is not
     * a failure: it mixes at its compile-time constant, which is what
     * every build before 2026-09-18 did. Say so once and carry on.
     */
    /*
     * A CARD RATE vsound CANNOT MIX AT, said as that - checked here
     * rather than read off the ioctl, because the module's -EINVAL
     * means both "out of range" and, from a module older than the
     * ioctl, "no such request". Before 2026-10-05 the first was
     * reported as the second ("this module mixes at a fixed rate").
     * -S is range-checked above, so this is a card that granted
     * something outside it on its own.
     */
    if (card_rate > 0
        && (card_rate < MIX_RATE_MIN || card_rate > MIX_RATE_MAX)) {
        int cur = 0;

        (void) ioctl(vsfd, VSOUND_IOC_MIXRATE, &cur);   /* 0 queries */
        fprintf(stderr, "vsoundd: %s runs at %d Hz, outside the %d-%d"
                        " vsound can mix at.\n"
                        "         The mix stays at %d Hz, so sound will"
                        " play at the wrong speed.\n",
                carddev, card_rate, MIX_RATE_MIN, MIX_RATE_MAX,
                cur > 0 ? cur : RATE);
    } else if (card_rate > 0) {
        int v = card_rate;

        if (ioctl(vsfd, VSOUND_IOC_MIXRATE, &v) == 0) {
            if (v != card_rate)
                fprintf(stderr, "vsoundd: asked to mix at %d Hz, module"
                                " chose %d\n", card_rate, v);
            else if (!quiet)
                fprintf(stderr, "vsoundd: mixing at %d Hz\n", v);

            /* THE WARNING THE USER ASKED FOR, and the case it exists
             * for: the card could not give what was wanted, so every
             * client is being resampled to something else. Silence
             * here is what made the original defect invisible. */
            if (v != RATE && want_rate <= 0)
                fprintf(stderr, "vsoundd: NOTE: %s runs at %d Hz, not"
                                " the usual %d\n", carddev, v, RATE);
            if (want_rate > 0 && v != want_rate)
                fprintf(stderr, "vsoundd: NOTE: asked for %d Hz, the"
                                " card gave %d - the hardware wins\n",
                        want_rate, v);
        } else if (errno == EBUSY) {
            fprintf(stderr, "vsoundd: cannot set the mix rate - a client"
                            " is already open\n");
        } else if (!quiet) {
            fprintf(stderr, "vsoundd: this module mixes at a fixed rate"
                            " (no MIXRATE ioctl)\n");
        }
    }

    if (outfile != NULL) {
        outfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd < 0)
            fprintf(stderr, "vsoundd: %s: %s (continuing)\n",
                    outfile, strerror(errno));
        else if (!quiet)
            fprintf(stderr, "vsoundd: also writing raw to %s\n", outfile);
    }

    /*
     * ARM RELEASE-ON-IDLE if -R was asked for. design/16 section 4.
     *
     * The kernel stays silent unless told: without this the read never
     * returns 0 on the idle transition, because its loop only exits
     * when it has data. Five attempts at the hanging note died on that
     * (design/16 section 3a).
     *
     * A kernel too old to know the ioctl fails here, and that is worth
     * saying rather than swallowing - the flag would otherwise appear
     * to work and never fire.
     *
     * BUT SAY WHAT WAS OBSERVED, NOT WHAT IT PROBABLY MEANS. This
     * printed "this module does not support release-on-idle" for ANY
     * failure, which is a diagnosis the code never made - and on
     * 2026-09-13 it sent a session to read the module first.
     *
     * The run was p3-67/run-20260913-220756. errno was EINVAL, and
     * vsound cannot produce it here: the pump guard returns -ENODEV
     * (vsound_dev.c:1813-1822) and the handler has only -EFAULT and 0
     * (:2257-2272). EINVAL is what the ioctl layer returns when the
     * fd's driver does not know the command - i.e. the fd was not
     * vsound's. The trace agrees: "dsp minor 35" where every healthy
     * run shows minor 3, so something else held the primary slot and
     * /dev/dsp was the real card. Nothing was wrong with the module.
     *
     * So ENODEV is the ONLY errno that means "too old" - that is the
     * guard's own rejection, and design/16 section 4 records it as the
     * fourth time an ioctl was added without its guard line. Anything
     * else is reported as what it is.
     */
    if (release) {
        int on = 1;
        if (ioctl(vsfd, VSOUND_IOC_RELEASE, &on) < 0) {
            if (errno == EBUSY) {
                /* ONE PUMP: the module gives the role to the first
                 * reader that reads or arms, and refuses the rest -
                 * two `-R' pumps side by side on 2026-09-14 split the
                 * audio and left one wrongly believing it was armed.
                 * A second pump has nothing useful to do; exit. */
                fprintf(stderr, "vsoundd: -R: another pump already"
                                " holds %s - kill it, or do not start"
                                " this one\n", vsdev);
                return 1;
            }
            if (errno == ENODEV) {
                fprintf(stderr, "vsoundd: -R: this module does not"
                                " support release-on-idle (%s) - the"
                                " card will be held as usual\n",
                        strerror(errno));
            } else {
                fprintf(stderr, "vsoundd: -R: could not arm"
                                " release-on-idle on %s (%s) - the card"
                                " will be held as usual\n",
                        vsdev, strerror(errno));
                fprintf(stderr, "vsoundd: -R: EINVAL here usually means"
                                " %s is not vsound - check the module"
                                " loaded before the card driver\n",
                        vsdev);
            }
            release = 0;
        } else if (!quiet) {
            fprintf(stderr, "vsoundd: -R: release-on-idle armed\n");
        }
    }

    if (!quiet)
        fprintf(stderr, "vsoundd: built %s %s\n", __DATE__, __TIME__);
    if (!quiet)
        fprintf(stderr, "vsoundd: pumping %s -> %s\n", vsdev, carddev);

    /* READY: the card is held and the mix rate is set, so a daemon may
     * now open vsound (design/54 6b). Not with no card - a capture-only
     * pump would take a daemon off a working card into silence. */
    if (cardfd >= 0)
        (void) vlhe_pump_state_write(carddev, VLHE_PUMP_READY);

    while (running) {
        n = read(vsfd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EBUSY) {
                /* The same refusal without -R: another reader is the
                 * pump. See the -R arming above. */
                fprintf(stderr, "vsoundd: another pump is already"
                                " draining %s - exiting\n", vsdev);
                break;
            }
            fprintf(stderr, "vsoundd: read: %s\n", strerror(errno));
            break;
        }
        if (n == 0) {
            /*
             * THE IDLE ACK - 2026-09-30. The kernel returns 0 from
             * read() while bytes are outstanding and nothing is ready,
             * precisely so this can run: ask the card where it is and
             * tell the kernel, with no write in between. Without it a
             * card whose position counter moves in coarse steps
             * (86Box's Solo-1) leaves the window full and unacked and
             * the pump parked in read() forever - vsound_dev.c has the
             * whole account at the matching change. Harmless on a card
             * that never needed it: an ack that retires nothing
             * changes nothing.
             */
            if (cardfd >= 0) {
                count_info icinfo;
                int iodelay;
                struct vsound_ack iack;

                memset(&iack, 0, sizeof iack);
                iack.written   = (__u32) written;
                iack.card_frag = (__u32) frag;
                if (ioctl(cardfd, SNDCTL_DSP_GETOPTR, &icinfo) == 0) {
                    iack.played = (__u32) (unsigned int) icinfo.bytes;
                    iack.flags |= VSOUND_ACK_PLAYED;
                }
                if (ioctl(cardfd, SNDCTL_DSP_GETODELAY, &iodelay) == 0
                    && iodelay >= 0) {
                    iack.odelay = (__u32) iodelay;
                    iack.flags |= VSOUND_ACK_ODELAY;
                }
                if (iack.flags != 0)
                    ioctl(vsfd, VSOUND_IOC_ACK, &iack);
            }
            /*
             * DESIGN B - release the card when the last client goes,
             * so the DRIVER's own teardown runs. design/16 section 4.
             *
             * On esssolo1 and four other PCI drivers close() runs
             * drain_dac(), stop_dac() and the DMA disable, which is
             * the only thing that stops the ring. Four attempts to
             * drain while HOLDING the device failed; this lets the
             * code that already works for every other application on
             * the system run for us too.
             *
             * WHY THE IDLE TICK. A zero-length read is the kernel
             * saying it has nothing mixed - the only moment we know we
             * are not truncating audio by letting go. Checking on a
             * tick that RETURNED data would race the tail of whatever
             * just closed.
             *
             * IT IS AN EDGE, NOT A LEVEL, and that distinction is the
             * whole of `had_client'. Releasing whenever nbusy == 0
             * would fire on EVERY idle tick - including at startup,
             * before any client has ever connected, and then again
             * every tick after each release-reacquire pair. The card
             * would be opened and closed continuously while nothing
             * plays, each cycle costing a PCI probe and a ~100 ms
             * window in which something else can take the device.
             *
             * So: arm on the first tick that sees a client, and fire
             * ONCE on the transition back to none. A daemon that has
             * never had a client never releases, and one that has
             * released does not release again until a client has come
             * and gone.
             */
            /*
             * A ZERO-LENGTH READ NOW MEANS "the last client has gone",
             * because -R armed it and the kernel only returns 0 in
             * that case. STAT is still consulted as a cross-check: it
             * costs one ioctl on a path that runs once per client, and
             * a release while a channel is somehow still busy would be
             * a truncation.
             */
            if (release && cardfd >= 0 && had_client
                && ioctl(vsfd, VSOUND_IOC_STAT, &st) == 0
                && st.nbusy == 0 && st.hard_ready == 0) {
                fprintf(stderr, "vsoundd: last client closed, releasing %s"
                                " (%lu KB played this session)\n",
                        carddev, written / 1024);
                close(cardfd);
                cardfd = -1;
                had_client = 0;         /* disarm: fire once per client */
                releases++;
                /*
                 * TELL THE KERNEL THE SIGNAL IS SPENT.
                 *
                 * Without this the read returns 0 again on every pass:
                 * `release_idle' is a level, and with no client left
                 * nothing can clear it, because clearing happens when a
                 * client STARTS. Until 2026-09-14 the read returned it
                 * instantly and this loop burned a full core - the
                 * 2026-09-13 spin. The kernel now sleeps before each -
                 * 10 ms since b87d728 (2026-09-30), 50 ms before - so
                 * the cost would be a wasted wake every 10 ms
                 * rather than a core, but the signal would still be
                 * pending for a release that already happened.
                 *
                 * SENT HERE, AFTER THE CLOSE SUCCEEDED, and only on
                 * this path - the branch above declines to release
                 * when STAT says a channel is busy or the hard buffer
                 * still holds bytes. Consuming the signal there would
                 * lose it on a release that did not happen, leaving
                 * the card held and the note hanging. Declining
                 * silently is correct: the level is still set, so the
                 * next read sleeps its 10 ms, returns 0, and we try
                 * again. THE SLEEP IS THE KERNEL'S, and the `continue'
                 * below relies on it: vsound_dev_read() tests the level
                 * only after its schedule_timeout(HZ/100; HZ/20 until
                 * b87d728) - since
                 * 2026-09-14; before that it tested above the sleep
                 * and a refusal here spun, design/25 B2. A sleep on
                 * this side would not be woken by a client starting.
                 *
                 * Failure is not worth acting on - an older module
                 * returns -ENODEV and simply keeps the old behaviour.
                 */
                (void) ioctl(vsfd, VSOUND_IOC_RELEASED);
            }
            continue;               /* nothing ready; the read blocks */
        }

        /*
         * ARM THE RELEASE. Reaching here means the kernel produced
         * audio, so a client exists - which is cheaper and more
         * reliable than asking STAT for nbusy on every tick.
         */
        if (release && !had_client) {
            had_client = 1;
            if (!quiet)
                fprintf(stderr, "vsoundd: client active, holding %s\n",
                        carddev);
        }

        /*
         * REACQUIRE, if -R released it and a client has come back.
         *
         * B1, a BOUNDED retry - design/16 section 4. The window is
         * ~100 ms (drain_dac's own timeout, computed from the driver),
         * so anything transient clears well inside RECLAIM_TRIES *
         * RECLAIM_MS. Beyond that the device has a real holder and
         * retrying forever would only burn CPU while silent.
         *
         * FAILING IS NOT FATAL. The old behaviour - exit on a failed
         * card open - would leave the machine silent until someone
         * restarted the daemon by hand, which is worse than the note
         * this option exists to fix. So we fall through with
         * cardfd < 0, which is the capture-only path the kernel
         * already supports: clients keep running and keep being
         * retired, they are simply not audible. The next idle-to-busy
         * transition tries again.
         *
         * The pump's open is O_NONBLOCK precisely so this can be
         * bounded (open_card clears the flag once it succeeds). A
         * blocking open here would hang the pump on whoever holds the
         * device - the bug that disguised contention for days.
         */
        if (release && cardfd < 0) {
            /*
             * ONE ATTEMPT PER TICK, not a sleep loop.
             *
             * A retry loop with usleep() here would stall the pump
             * while the kernel's ring keeps filling with nobody
             * draining it - paying for the outage twice. The ticks are
             * already arriving (this branch is reached because a
             * client produced audio), so counting them gives the same
             * bounded budget for free and the pump keeps servicing the
             * read in between.
             *
             * FAILING IS NOT FATAL. The old behaviour - exit on a
             * failed card open - would leave the machine silent until
             * someone restarted the daemon by hand, which is worse
             * than the note this option exists to fix. We fall through
             * with cardfd < 0, which is the capture-only path the
             * kernel already supports: clients keep running and keep
             * being retired, they are simply not audible, and the next
             * tick tries again.
             *
             * The pump's open is O_NONBLOCK precisely so this can be
             * bounded (open_card clears the flag once it succeeds). A
             * blocking open here would hang the pump on whoever holds
             * the device - the bug that disguised contention for days.
             */
            /* NO RATE ARGUMENT ON A REACQUIRE. Clients are running
             * by definition here, and the module refuses a mix-rate
             * change while any channel is open - correctly, since
             * each one computed its ratio from the old value. The
             * card gets the same request it got at startup. */
            cardfd = open_card(carddev, &frag, 1, want_rate, NULL, 1);
            /* SAVED IMMEDIATELY. `open_card()' prints its own
             * message on the way out, and anything it calls may
             * clobber errno before the report below runs. */
            reclaim_errno = (cardfd < 0) ? errno : 0;
            if (cardfd >= 0) {
                reacquires++;
                /* Not gated on -q: a reacquisition that took several
                 * ticks means something contended for the card, and
                 * that is exactly what a log is read for afterwards.
                 * `reclaim' is the count of ticks it took. */
                fprintf(stderr, "vsoundd: reacquired %s after %d tick%s"
                                " (release %lu, reacquire %lu)\n",
                        carddev, reclaim, reclaim == 1 ? "" : "s",
                        releases, reacquires);
                reclaim = 0;
                said_lost = 0;
            } else if (++reclaim >= RECLAIM_TICKS && !said_lost) {
                /* Say it ONCE per outage, not once per tick, and arm
                 * it again on the next success so a second outage is
                 * also reported. B2 - telling the user properly - is
                 * cleanup-step work; this is the honest minimum. */
                said_lost = 1;
                /*
                 * SAY WHY, DO NOT ASSERT A CAUSE - 2026-09-28.
                 *
                 * This read "something else holds it", which
                 * describes EBUSY and is a guess: the code never
                 * looked at errno. On 86Box the same evening it
                 * printed that line while the real error was
                 * ENOENT - eleven "No such file or directory"
                 * messages immediately above it in DAEMON.LOG -
                 * so it named a busy device when the NODE DID NOT
                 * EXIST, sending a reader after the wrong problem.
                 *
                 * EBUSY and ENOENT want different actions: one is
                 * "close the other program", the other is "the
                 * card driver is not loaded, or its node is gone".
                 * Printing strerror() lets the reader tell them
                 * apart, and the two common cases get a sentence
                 * each.
                 */
                fprintf(stderr, "vsoundd: cannot reacquire %s: %s."
                                " Audio is silent until it is"
                                " available.\n", carddev,
                        strerror(reclaim_errno));
                if (reclaim_errno == ENOENT)
                    fprintf(stderr, "vsoundd: the node is GONE - the"
                                    " card driver was probably"
                                    " unloaded, or never created"
                                    " it.\n");
                else if (reclaim_errno == EBUSY)
                    fprintf(stderr, "vsoundd: something else holds"
                                    " it - close that program and"
                                    " audio resumes.\n");
            }
        }

        if (cardfd >= 0) {
            /* THE BLOCKING WRITE IS THE PACING. It returns when the
             * card has room, which is what stops us running ahead. */
            struct timeval t0, t1;
            long ms;

            gettimeofday(&t0, (struct timezone *) 0);
            w = write(cardfd, buf, (size_t) n);
            gettimeofday(&t1, (struct timezone *) 0);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "vsoundd: write: %s\n", strerror(errno));
                break;
            }

            /*
             * HOW LONG THE CARD MADE US WAIT, added 2026-08-31.
             *
             * The kernel trace shows the hard buffer 95% saturated
             * (`out 16384' on 10206 of 10740 drains) and the card
             * holding ~83 ms - but nothing said whether that is the
             * CARD being slow or this process not being scheduled.
             * This write is the only place the difference shows.
             *
             * Reported as a running summary rather than per write:
             * one line every PUMP_REPORT writes, plus an immediate
             * line for anything over PUMP_SLOW_MS, capped so a
             * genuinely slow card cannot flood DAEMON.LOG.
             */
            ms = (t1.tv_sec - t0.tv_sec) * 1000
                 + (t1.tv_usec - t0.tv_usec) / 1000;
            if (ms < 0)
                ms = 0;
            pump_calls++;
            if ((unsigned long) ms > pump_worst)
                pump_worst = (unsigned long) ms;
            if (ms >= PUMP_SLOW_MS) {
                pump_slow++;
                if (pump_said < 5) {
                    pump_said++;
                    fprintf(stderr, "vsoundd: card write blocked %ld ms"
                                    " (%d bytes)\n", ms, (int) n);
                }
            }
            if (pump_calls % PUMP_REPORT == 0)
                fprintf(stderr, "vsoundd: %lu writes, %lu slow (>=%d ms),"
                                " worst %lu ms, %lu KB out\n",
                        pump_calls, pump_slow, PUMP_SLOW_MS, pump_worst,
                        (written + (unsigned long) w) / 1024);
        } else {
            /* Capture-only: nothing paces us but the kernel's read,
             * which blocks until the mixer has produced something. That
             * is slower than real time by design - this is a
             * diagnostic, not a way to listen. */
            w = n;
        }
        written += (unsigned long) w;

        if (outfd >= 0)
            write(outfd, buf, (size_t) w);

        /*
         * TELL THE KERNEL WHAT THE CARD DID. This is the whole
         * protocol. GETOPTR is a monotonic byte count; GETODELAY is
         * what is still queued. The kernel uses only the DELTA of the
         * first and clamps it, so a counter that resets cannot corrupt
         * anything - and it uses the second to correct its own
         * accounting downward when the two disagree.
         *
         * GETOPTR IS UNSIGNED IN EVERYTHING BUT THE STRUCT - design/25
         * B9c, fixed 2026-09-16. Every 2.2 driver keeps this count as
         * an `unsigned' and adds to it forever: `esssolo1.c:201'
         * (the Acer's card), and `es1371', `cmpci' and `sonicvibes'
         * alike; 2.4's esssolo1 is unchanged. But `count_info.bytes'
         * is declared `int' (`soundcard.h:591'), so at 2^31 bytes -
         * 3.4 hours of CD audio, 13.5 of quake's 11025 stereo - the
         * field the driver copies into goes NEGATIVE while the card's
         * own counter is still perfectly correct.
         *
         * This used to test `cinfo.bytes >= 0' and skip the report
         * when it failed, so past that point the kernel retired on
         * odelay alone. That works on every card here, and on a card
         * without GETODELAY it would stop retiring altogether and hit
         * the stall discard.
         *
         * THE SIGN TEST IS THE BUG: the question is meaningless for a
         * value that is really unsigned. Casting through `unsigned
         * int' recovers the driver's number exactly, and the kernel
         * already takes a wrap-safe unsigned difference
         * (`vsound_drain.c:253-255'), so the genuine wrap at 2^32 -
         * 6.8 hours - costs nothing either: the subtraction is exact
         * across it. FreeBSD's reference avoids the question by
         * keeping `u_int64_t total, prev_total' (`buffer.h:49') and
         * working only in differences, which is what we do.
         *
         * GETODELAY's test below STAYS. That one is a queue depth, a
         * small positive number by nature, and a negative one is a
         * driver fault rather than a wrap.
         */
        memset(&ack, 0, sizeof ack);
        ack.written   = (__u32) written;
        ack.card_frag = (__u32) frag;

        if (cardfd >= 0) {
            if (ioctl(cardfd, SNDCTL_DSP_GETOPTR, &cinfo) == 0) {
                /* Unsigned, not tested for sign - see above. */
                ack.played = (__u32) (unsigned int) cinfo.bytes;
                ack.flags |= VSOUND_ACK_PLAYED;
            }
            if (ioctl(cardfd, SNDCTL_DSP_GETODELAY, &odelay) == 0
                && odelay >= 0) {
                ack.odelay = (__u32) odelay;
                ack.flags |= VSOUND_ACK_ODELAY;
            }
        } else {
            /* No card, so nothing measured it. Report only `written',
             * and the kernel retires on that alone - which is right:
             * in capture-only mode the file IS the consumer. */
            ack.played = (__u32) written;
            ack.flags |= VSOUND_ACK_PLAYED;
        }

        /* A failed ack is not fatal: the kernel falls back to its timer
         * and keeps playing. Nothing is worth stopping audio over. */
        ioctl(vsfd, VSOUND_IOC_ACK, &ack);
    }

    if (!quiet)
        fprintf(stderr, "vsoundd: stopping, %lu bytes written\n", written);

    /*
     * THE HEADROOM LINE, at the one moment the figure is final.
     *
     * clip_passes is the answer `design/06' section 4g asks for: mix
     * passes in which a sample saturated. Zero means the levels had
     * room; a steady count means they did not, and the per-client
     * BOOST option in `design/09' waits on exactly this. Measured for
     * the first time on 2026-09-01: FOUR clients at their own default
     * levels gave 3862 passes, so there is no headroom to spare
     * (tests/logs/2026-09-01-86box-clipcounter-4clients).
     *
     * `underruns' IS DELIBERATELY NOT PRINTED HERE, and this is the
     * whole reason the comment is long.
     *
     * It was, for one run, and it read `3478864 underruns' beside a
     * genuine measure - which invites reading it as three and a half
     * million audio failures. IT IS NOT A FAULT COUNT.
     * vsound_drain.c:204 increments on every drain attempt that finds
     * nothing ready, and its own comment says so: "an idle device is
     * not an anomaly". The pump's read polls at 20 Hz AND loops
     * internally, so a quiet or idle device increments it constantly.
     *
     * Nor is gating it on nrunning > 0 a fix: nrunning is device-wide
     * and counts channels that have been STARTED, so one active client
     * makes every empty drain count even where the emptiness is
     * entirely normal - the mixer produces one tick's worth per tick
     * and the pump asks more often than that.
     *
     * The counter stays in VSOUND_IOC_STAT, where it is honest about
     * what it counts and a tool can read it deliberately. It is the
     * PRINTING beside a real measure that misleads. Renaming it to
     * something like `drain_empty' is a cleanup-step item - see
     * `design/09', and `design/10' divergence 4, which calls it the
     * xrun counter it is not.
     */
    if (!quiet && ioctl(vsfd, VSOUND_IOC_STAT, &st) == 0)
        fprintf(stderr, "vsoundd: %lu clipped mix pass%s\n",
                (unsigned long) st.clip_passes,
                st.clip_passes == 1 ? "" : "es");
    /*
     * THE -R SUMMARY, always printed when -R was asked for. A run
     * where the count is 0 is the interesting one: it means the
     * release never fired, and the note would still hang - which is
     * indistinguishable from "the fix does not work" unless the log
     * says which happened.
     */
    if (release)
        fprintf(stderr, "vsoundd: -R: %lu release%s, %lu reacquire%s%s\n",
                releases,   releases   == 1 ? "" : "s",
                reacquires, reacquires == 1 ? "" : "s",
                cardfd < 0 ? ", card NOT held at exit" : "");

    if (outfd >= 0)
        close(outfd);
    if (cardfd >= 0)
        close(cardfd);
    close(vsfd);

    /* AT A CLEAN EXIT ONLY, so a pid file left behind means the pump
     * DIED rather than stopped - the third state the Status page
     * reports as "stale pid file - it died". */
    vlhe_status_remove_pidfile("vsoundd");
    return 0;
}
