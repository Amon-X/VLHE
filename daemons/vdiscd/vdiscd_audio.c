/*
 * vdiscd_audio.c - the CDDA child.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/07-vsound.md sections 3.1, 3.1a and 3.1b. It does one thing:
 * read CDDA from the image and write it to /dev/dsp, which is vsound.
 *
 * IT IS A CLIENT NOW, NOT A DEVICE HOLDER. `vcdd' opened the real card
 * and kept it, because holding the device was how you got it. vsound
 * holds /dev/dsp now, so this opens a CHANNEL like quake or lxdoom
 * does - and closes it when the track ends (section 3.1a). A disc
 * sitting in a drive must not spend one of four channels on silence.
 *
 * WHAT IS NOT HERE, and section 3.1b says why: no collector ring, no
 * shared memory, no carry buffer, no per-fragment metadata, no format
 * negotiation aimed at a card. All of that existed because unmixed
 * audio crossed a process boundary. vsound mixes now.
 *
 * WHY IT IS STILL A SEPARATE PROCESS - and the reason changed. The old
 * one forked because writing to the card took 46 ms and this spent
 * 89.89% of its life inside write(). Writing to vsound is bounded by
 * one tick. What remains is CONTAINMENT: the image is read with
 * blocking, uninterruptible I/O, and if that hangs on failing media the
 * process is in D state and cannot be signalled. Forked, the parent
 * keeps serving block reads and the guest's filesystem keeps working.
 * Inline, the guest blocks on its own filesystem.
 *
 * C89: GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>           /* gettimeofday, for the write timing */
#include <sys/soundcard.h>

#include "vdisc.h"
#include "vsound.h"
#include "vdiscd_play.h"
#include "vlhe_status.h"        /* vlhe_resolve_out - design/43 part A */

/*
 * WHERE THE MIX GOES, AND IT IS NOT ALWAYS /dev/dsp - 2026-09-24.
 *
 * vsound takes the node the USER PICKED (design/38), so on a machine
 * where that pick is /dev/dsp2 this daemon opening /dev/dsp reaches
 * whatever the shuffle left there - a real card, or nothing. The
 * default stays /dev/dsp because that is the single-card answer and
 * the common one; `vdiscd -o NODE' is how the plan says otherwise.
 */
#define DSP_DEV_DEFAULT "/dev/dsp"

static const char *dsp_dev = DSP_DEV_DEFAULT;

/*
 * SAID ONCE, NOT ONCE PER PLAY. With `-o @VSOUND@' and vsound not
 * yet loaded, every play command would otherwise repeat the same
 * line - and KsCD sends one per track. Cleared on a successful
 * resolve so a LATER wait is reported again.
 */
static int said_waiting;
/* The last open_channel() failure, so a retry failing the same way is
 * not printed again (design/36 row 139). */
static int  open_failing;
static int  open_fail_errno;
static char open_fail_dev[VLHE_OUT_MAX];

/*
 * WHAT `-o' LAST RESOLVED TO - so a later open can tell whether the
 * answer has MOVED.
 *
 * With `@VSOUND:@CARD@@' the answer changes when vsound is loaded or
 * unloaded, and an already-open channel would otherwise keep playing
 * to whatever it opened first. See the rebind check in CMD_PLAY.
 *
 * EMPTY MEANS "NOTHING TO COMPARE AGAINST", NOT "IT MOVED - 2026-09-27,
 * and reading it the other way was a regression.
 *
 * THIS IS PER-CHILD STATE AND CANNOT BE ANYTHING ELSE. The audio
 * child is a fork() (`vdiscd_play.c:136'), and `vdiscd_play_stop()'
 * KILLS it - so a stop/play cycle gets a FRESH child with this
 * static back at "". Measured on 86Box 2026-09-27: two plays of the
 * same track, and both logged ``resolved ... (was `')''.
 *
 * So the rebind check below MUST skip an empty value. Comparing
 * "/dev/dsp3" against "" differs, which closed and reopened the
 * channel on the FIRST play of every child - and a close DRAINS,
 * so the user heard the previous play's tail before the track
 * started. See tests/logs/2026-09-27-cdg-close-attribution/.
 */
static char dsp_resolved[VLHE_OUT_MAX];

/*
 * CLOSE AND THROW AWAY WHAT IS QUEUED - 2026-09-27.
 *
 * A BARE close() ON AN OSS DEVICE DRAINS: it plays out whatever is
 * still buffered before it returns. That is right when a track ended
 * on its own and wrong every other time, and this file had six bare
 * closes and no reset at all.
 *
 * THE SYMPTOM IT CAUSED: "it plays a bit like whatever was in a
 * buffer and then plays the track normally" (the user, 2026-09-27, in
 * grip and KsCD). ~110 ms of the PREVIOUS play draining as the
 * channel was closed and reopened.
 *
 * SNDCTL_DSP_RESET DISCARDS instead, which is what a stop, an error
 * or a rebind wants. It also makes the close immediate rather than
 * blocking for the buffer's duration inside the command path.
 *
 * THE TRACK-END CLOSE DELIBERATELY DOES NOT CALL THIS - see there.
 */
static void
close_now(int fd, const char *why, unsigned int lba)
{
    fprintf(stderr, "vdiscd/audio: close (%s) at lba %u\n", why, lba);
    /* A failure is not worth reporting: the close below releases the
     * device either way, and a card that refuses RESET simply drains
     * as it did before. */
    (void) ioctl(fd, SNDCTL_DSP_RESET, 0);
    close(fd);
}

void
vdiscd_audio_set_device(const char *node)
{
    if (node != NULL && *node != '\0')
        dsp_dev = node;
}
#define CDDA_RATE       44100
#define CDDA_CHANNELS   2
#define RAW_SECTOR      2352            /* one CDDA frame              */
#define CHUNK_FRAMES    8               /* ~2.1 ms of audio per read    */

/* The commands the parent sends down the pipe: struct vdiscd_cmd and
 * the CMD_* opcodes, in vdiscd_play.h - one struct per command, one
 * read (design/26 B11). */

static volatile int running = 1;

/*
 * The dsp fd, visible to the signal handler.
 *
 * SETTING A FLAG IS NOT ENOUGH ON ITS OWN. The child spends most of its
 * life blocked in write() to /dev/dsp - 18816 bytes into a 4096-byte
 * channel buffer - and that write does not return because `running'
 * changed. It returns when the mixer has drained enough, which is over
 * 100 ms away and longer under load.
 *
 * So SIGTERM CLOSES THE DEVICE, which makes the blocked write fail with
 * EBADF and the loop exit at once. On 2026-08-25 a child was declared
 * wedged for doing nothing worse than a normal write.
 */
static volatile int dsp_fd = -1;

static void
on_term(int sig)
{
    int fd = dsp_fd;

    (void) sig;
    running = 0;

    /*
     * close() is not on the strict list of async-signal-safe functions
     * that a handler may call, but it is safe in practice here and is
     * what makes the blocked write return. The alternative - waiting
     * for the write to finish on its own - is what caused the bug.
     *
     * AND THE RESET IS WHAT MAKES A STOP SILENT - 2026-09-27. THIS
     * IS THE CLOSE THAT RUNS ON EVERY STOP, and it was the last bare
     * one in the file.
     *
     * `vdiscd_play_stop()' sends CMD_STOP and then immediately calls
     * `vdiscd_child_stop()', which closes the command pipe and
     * SIGTERMs the child (`vdiscd_child.c'). The child is blocked in
     * write() the whole time, so it NEVER READS CMD_STOP - it dies
     * here instead. Measured on 86Box 2026-09-27: three plays, three
     * stops, and not one of the six closes in the main loop ever
     * logged. Fixing those first fixed nothing, because none of them
     * runs.
     *
     * A BARE close() DRAINS - it plays out what is queued. That is
     * the "bit of whatever was in a buffer" the user heard before
     * each track. SNDCTL_DSP_RESET discards instead, and it also
     * does this handler's own job BETTER: it makes the blocked write
     * return at once rather than after the buffer empties.
     *
     * ioctl() is not on the async-signal-safe list either, for the
     * same reason close() is not, and is safe here on the same
     * grounds - see the paragraph above.
     */
    if (fd >= 0) {
        dsp_fd = -1;
        (void) ioctl(fd, SNDCTL_DSP_RESET, 0);
        close(fd);
    }
}

/*
 * Open a channel on vsound for this track.
 *
 * PER TRACK, NOT PER DISC - section 3.1a. Opened when playback starts
 * and closed when it stops, so an idle disc holds nothing.
 *
 * The negotiation is CDDA's own format and nothing else. `vcdd' had a
 * SETFRAGMENT block here tuning the card's geometry to cut 372 ms of
 * buffering; that is vsound's business now, and asking for a geometry
 * would only size OUR channel buffer - which section 3.1a wants small,
 * not large.
 */
static int
open_channel(void)
{
    int  fd, v;
    char dev[VLHE_OUT_MAX];

    /*
     * RESOLVE `-o' HERE, NOT AT STARTUP - design/43 part A.
     *
     * `dsp_dev' may be the literal "@VSOUND@", and this runs on
     * every play, so a vsound loaded after this daemon started is
     * found without a restart. 0 means "not loaded": the caller
     * retries rather than falling back to a real card, which would
     * hold that card for a WHOLE TRACK and give everything else
     * EBUSY.
     */
    /*
     * A RETRY THAT FAILS THE SAME WAY IS SAID ONCE - design/36 row 139,
     * 2026-10-03. The idle branch calls this every 500 ms while a play
     * waits for a device, and each call printed the "resolved" line and
     * the open error again: about 30 lines in one wait on 86Box. Now a
     * failure is printed when it is NEW - a different device or a
     * different errno - and the "resolved" line is skipped while the
     * same device keeps failing. A success clears it.
     */
    switch (vlhe_resolve_out(dsp_dev, dev, sizeof dev)) {
    case 1:
        /* SAY WHAT IT RESOLVED TO, AND WHAT IT WAS BEFORE - 2026-09-27.
         *
         * `dsp_resolved' is static and starts EMPTY, so the first
         * rebind check compares against "" and would close and reopen
         * on the first play of a run - logging `output moved  -> X'
         * with a blank left side. Printing the previous value here
         * makes that visible rather than leaving it to be inferred. */
        if (!open_failing || strcmp(dev, open_fail_dev) != 0)
            fprintf(stderr, "vdiscd/audio: resolved `%s' -> %s (was `%s')\n",
                    dsp_dev, dev, dsp_resolved);
        strcpy(dsp_resolved, dev);      /* same size, checked by resolve */
        break;
    case 0:
        if (!said_waiting) {
            fprintf(stderr, "vdiscd/audio: waiting for vsound and its"
                            " pump (vsoundd) - audio cannot play until"
                            " it is ready\n");
            said_waiting = 1;
        }
        return -1;
    default:
        fprintf(stderr, "vdiscd/audio: cannot resolve output device"
                        " `%s'\n", dsp_dev);
        return -1;
    }
    said_waiting = 0;

    /*
     * O_NONBLOCK ON THE OPEN, CLEARED THE INSTANT IT SUCCEEDS -
     * 2026-09-28, and both halves of that matter.
     *
     * WITHOUT IT A PCI CARD'S OPEN BLOCKS. `esssolo1' loops on
     * schedule() while the device is taken and only returns EBUSY
     * when this flag is set (`esssolo1.c', and CLAUDE.md section 5
     * records the days it cost on the old tree). es1371, es1370,
     * cmpci, sonicvibes and maestro are all the same shape. So the
     * honest failure - "something else has the card" - is only
     * available with the flag.
     *
     * AND CLEARING IT IS NOT OPTIONAL. `audio.c:225' tests
     * `file->f_flags & O_NONBLOCK' on EVERY WRITE, so a flag left
     * set turns blocking writes into EAGAIN - and the blocking
     * write IS this daemon's pacing. That would spin instead of
     * being throttled by the card, which is a worse bug than the
     * one this fixes. `vsoundd' has done it this way since it was
     * written; this is the same two lines.
     *
     * THE LEGACY PATH IS UNAFFECTED EITHER WAY: ad1848 and the
     * `sb' route have no wait loop and no O_NONBLOCK reference at
     * all - they already return EBUSY unconditionally.
     */
    fd = open(dev, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        int e = errno;

        /*
         * THE ERRNOS THAT MEAN BUSY. Surveyed across the 2.2 sound
         * drivers: EBUSY for esssolo1/es1371/es1370/cmpci/sonicvibes
         * and the legacy path, EWOULDBLOCK (EAGAIN) for maestro.
         * CORRECTED 2026-10-03 (design/54 6c): this said EAGAIN for
         * emu10k1 v0.20a, but neither emu10k1 driver is exclusive for
         * playback - a second open succeeds. i810 and trident say
         * ENODEV with every channel taken, which is not matched here
         * because soundcore says the same for a minor with no driver.
         */
        if (!open_failing || e != open_fail_errno
            || strcmp(dev, open_fail_dev) != 0) {
            fprintf(stderr, "vdiscd/audio: %s: %s\n", dev, strerror(e));
            if (e == EBUSY || e == EAGAIN || e == EWOULDBLOCK)
                fprintf(stderr, "vdiscd/audio: all channels are in use"
                                " - retrying quietly\n");
        }
        open_failing    = 1;
        open_fail_errno = e;
        strncpy(open_fail_dev, dev, sizeof open_fail_dev - 1);
        open_fail_dev[sizeof open_fail_dev - 1] = '\0';
        errno = e;
        return -1;
    }
    open_failing = 0;

    /* AND OFF AGAIN: from here the write blocking IS the flow
     * control. See the comment on the open. */
    fcntl(fd, F_SETFL, 0);

    /*
     * IS THIS vsound? - the check vmidid has had since row 53 and
     * this daemon never did. VSOUND_IOC_STAT is a read-only ioctl
     * only vsound answers, so an error means a real card.
     *
     * IT MATTERS MORE HERE THAN FOR THE SYNTH. vmidid releases
     * between phrases; this holds the device for a whole TRACK, so
     * anything else that wants it waits minutes. And apply_volume()
     * below fails silently on a card - VSOUND_IOC_CHANS returns an
     * error and it just returns - so KsCD's slider does nothing
     * with no line saying why. Said ONCE per open, not per play.
     */
    {
        struct vsound_stat st;

        if (ioctl(fd, VSOUND_IOC_STAT, &st) != 0)
            fprintf(stderr, "vdiscd/audio: %s is NOT vsound - a real sound"
                            " card. Nothing else can use it while a track"
                            " plays, and the volume control will do"
                            " nothing. Load vsound for mixing.\n", dev);
    }

    v = AFMT_S16_LE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &v) < 0 || v != AFMT_S16_LE) {
        fprintf(stderr, "vdiscd/audio: format is 0x%x, wanted S16_LE\n", v);
        close(fd);
        return -1;
    }

    v = CDDA_CHANNELS;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) < 0 || v != CDDA_CHANNELS) {
        fprintf(stderr, "vdiscd/audio: %d channels, wanted %d\n",
                v, CDDA_CHANNELS);
        close(fd);
        return -1;
    }

    v = CDDA_RATE;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &v) < 0) {
        fprintf(stderr, "vdiscd/audio: SPEED failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (v != CDDA_RATE)
        fprintf(stderr, "vdiscd/audio: got %d Hz, wanted %d - CDDA will"
                        " be resampled\n", v, CDDA_RATE);

    return fd;
}

/*
 * SET THIS CHANNEL'S VOLUME on an open fd.
 *
 * VSOUND_IOC_VOL addresses a channel by INDEX and we do not know ours,
 * so ask for the list and match on our own pid. Cheap, and only on an
 * actual volume change or a channel open - KsCD sends one at startup
 * and then only when the user moves the slider.
 *
 * Factored out 2026-09-14 (design/26 B5) so the SAME level can be
 * applied at two moments: when a VOLUME command arrives while the
 * channel is open, and right after open_channel() when a level arrived
 * while it was not. The channel is closed between tracks and while
 * stopped, and KsCD sets its level immediately BEFORE each play - so
 * "apply to the open fd or drop it" dropped it on every play.
 */
static void
apply_volume(int fd, int v)
{
    struct vsound_chanlist cl;
    int k;

    if (fd < 0 || v < 0)
        return;
    if (ioctl(fd, VSOUND_IOC_CHANS, &cl) < 0)
        return;
    for (k = 0; k < (int) (sizeof cl.chan / sizeof cl.chan[0]); k++) {
        if (cl.chan[k].pid == (unsigned int) getpid()) {
            struct vsound_vol sv;
            memset(&sv, 0, sizeof sv);
            sv.index = cl.chan[k].index;
            sv.pid   = (unsigned int) getpid();
            sv.vol   = (unsigned int) v;
            if (ioctl(fd, VSOUND_IOC_VOL, &sv) < 0)
                fprintf(stderr, "vdiscd/audio: setting volume:"
                                " %s\n", strerror(errno));
            break;
        }
    }
}

/*
 * Read one command from the pipe, if there is one.
 *
 * Non-blocking: this must not stop the audio flowing. Returns 0 when
 * nothing is waiting, -1 when the pipe has closed - which is the
 * parent's way of saying exit.
 */
static int
poll_cmd(int cmd_fd, struct vdiscd_cmd *out)
{
    int n = (int) read(cmd_fd, out, sizeof *out);

    if (n == (int) sizeof *out)
        return 1;
    if (n > 0) {
        /* Cannot happen for a writer that sends whole structs under
         * PIPE_BUF; say so rather than parse half a command. */
        fprintf(stderr, "vdiscd/audio: short command read (%d bytes)"
                        " - ignored\n", n);
        return 0;
    }
    if (n == 0)
        return -1;                      /* EOF: the parent is gone */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
    if (errno == EINTR)
        return 0;
    return -1;
}

/*
 * The child's whole life.
 *
 * `read_frames' is a callback so this file has no opinion about image
 * formats - the .ccd, .cue and .iso backends are the caller's business
 * and are the one part of the old tree section 7 names as safe to carry
 * across unchanged.
 */
int
vdiscd_audio_child(int cmd_fd,
                   int (*read_frames)(void *ctx, unsigned int lba,
                                      unsigned int nframes, void *buf),
                   void (*report)(void *ctx, unsigned int lba, int status,
                                  int track),
                   int (*track_at)(void *ctx, unsigned int lba),
                   void *ctx)
{
    unsigned char buf[RAW_SECTOR * CHUNK_FRAMES];
    unsigned int lba = 0, end = 0;
    unsigned int reported = 0;
    int paused = 0, playing = 0, track = 0;
    /* THE LAST LEVEL ASKED FOR, applied to every channel this child
     * opens from then on. -1 until one arrives. design/26 B5. */
    int vol = -1;
    /* Has this play written anything yet? A play that never writes is
     * the failure mode of 2026-08-27 and is otherwise invisible. */
    int wrote_any = 0;
    /*
     * PROGRESS INSTRUMENTATION, added 2026-08-31.
     *
     * THE BUG THIS EXISTS FOR: with lxdoom running, CD audio stops
     * completely and THE TRACK TIMER DOES NOT ADVANCE, then resumes
     * cleanly when lxdoom quits. Reproduced on the 26 Aug build and on
     * the current one, so it is not a regression.
     *
     * Nothing recorded the child's POSITION over time. The kernel trace
     * sees bytes arriving at a channel; this file announced only the
     * FIRST write of each play. So "the CD is not advancing" - the one
     * thing visible on screen - was measurable nowhere, and four
     * separate readings of the kernel trace tried to infer it from
     * write volume and got it wrong.
     *
     * `lba' is the answer directly: if it climbs while nothing is
     * heard, the child is fine and the loss is downstream; if it stops,
     * the child is where to look.
     */
    unsigned int logged = 0;            /* lba at the last progress line */
    unsigned long wr_calls = 0;         /* write() calls this play      */
    unsigned long wr_short = 0;         /* ...that returned short       */
    unsigned long wr_bytes = 0;         /* bytes accepted this play     */
    unsigned long wr_slow  = 0;         /* ...that took over WR_SLOW_MS */
    unsigned long wr_worst = 0;         /* worst single write, ms       */
#define dsp dsp_fd      /* one variable, so the handler and the loop
                         * cannot disagree about which fd is open */
    struct vdiscd_cmd m;

    /*
     * HOW OFTEN TO REPORT.
     *
     * CDROMSUBCHNL is polled every frame by some games, but the KERNEL
     * answers those from its cached value - this only has to keep that
     * value fresh. Each report is a TOC lookup (a walk of at most 99
     * entries, in memory) and one ioctl on an fd we already hold.
     *
     * Reporting per chunk instead would be ~470 a second at
     * CHUNK_FRAMES=8, which is pure overhead.
     *
     * WAS 75 - ONE REPORT A SECOND - AND THAT MADE THE CD+G LYRICS
     * ADVANCE IN ONE-SECOND STEPS, 2026-09-29. The reasoning for 75
     * was "nothing can observe a position finer than the 75
     * frames/second the format has", which confuses the UNIT with
     * the RATE: an LBA cannot be finer than 1/75 s, but reporting
     * one once a second is 75x coarser than the format allows. That
     * was right while the only consumer was a game's time display.
     *
     * THE CD+G VIEWER READS THIS AS A DECODE CURSOR, not as a
     * readout. It polls every CDG_POLL_MS and decodes the gap since
     * last time, so a cursor that moves once a second made it
     * decode a second of packets in a burst and then sit still for
     * the polls in between - while the format is 300 packets a
     * second and the lyric strip is the one region changing
     * continuously. `vlhe_mod_cdg.c' predicted exactly this and
     * named this constant as where to look.
     *
     * 5 FRAMES IS A FIFTEENTH OF A SECOND, matching CDG_POLL_MS 67.
     * **THE TWO ARE A PAIR AND MUST MOVE TOGETHER** - a viewer
     * polling faster than this finds the cursor unmoved and decodes
     * nothing, so the picture would step at THIS rate however often
     * it ran.
     *
     * It was 19 (a quarter second) for the few hours between the
     * two changes, and 75 (one second) before that.
     *
     * FIFTEEN REPORTS A SECOND, each a TOC walk of at most 99
     * entries in memory plus one ioctl on an fd we already hold,
     * against ~470 writes a second. Measured nowhere slower than
     * 86Box yet: the P1 (Pentium 233) is untestable at the moment,
     * and it is the machine where this would show if anywhere.
     */
#define REPORT_FRAMES   5               /* a fifteenth of a second */

    /*
     * HOW OFTEN TO LOG PROGRESS. One line per LOG_FRAMES of audio -
     * 75 frames is one second, so 375 is one line every five seconds.
     * Sparse on purpose: this goes to DAEMON.LOG alongside vsoundd's
     * output and has to stay readable across a whole session, and the
     * question it answers ("is the position moving at all") does not
     * need fine resolution.
     */
#define LOG_FRAMES      375             /* five seconds of CDDA */

    /*
     * A WRITE WORTH COMPLAINING ABOUT. The child is single-threaded -
     * read, write, read, write - so time inside write() is time NOT
     * spent reading ahead. At 44100 stereo a CHUNK_FRAMES chunk is
     * ~107 ms of audio, so a write blocking longer than this is the
     * child losing ground in real time.
     *
     * The old tree measured exactly this and it was the useful number:
     * `vcdd: write 59.48% n=17 avg=177976us' in
     * tests/logs/2026-08-21-peak/MIXTEST.LOG. That instrumentation was
     * not carried across to vdiscd.
     */
#define WR_SLOW_MS      50

    dsp_fd = -1;

    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);
    signal(SIGPIPE, SIG_IGN);

    fcntl(cmd_fd, F_SETFL, O_NONBLOCK);

    while (running) {
        int rc = poll_cmd(cmd_fd, &m);
        int n, want, wrote;

        if (rc < 0)
            break;                      /* parent gone */

        if (rc == 1) {
            switch (m.op) {
            case CMD_PLAY:
                lba   = m.lba;
                end   = m.end;
                track = m.arg;
                /*
                 * HAS THE OUTPUT MOVED UNDER US? - found on target
                 * 2026-09-26, and it is why a rebind that WORKS
                 * still did not happen.
                 *
                 * THE CHANNEL IS NOT CLOSED AT A TRACK BOUNDARY.
                 * `lba >= end' closes it, but a CD player asking
                 * for continuous play sends ONE command covering
                 * the rest of the disc - grip sent
                 * `play 238288..277875', the lead-out - so track 10
                 * became track 11 in the same open channel and
                 * `end' was never reached. A whole disc played from
                 * track 1 is ONE open channel.
                 *
                 * SO A vsound LOADED MID-DISC WAS NEVER PICKED UP.
                 * The user: *"it didnt rebind on track change I had
                 * to close grip and start it open"* - which forced
                 * a CMD_STOP, which does close.
                 *
                 * RESOLVING COSTS ONE /proc READ PER PLAY COMMAND,
                 * which is per track at worst. Cheap enough to do
                 * unconditionally rather than guess when it might
                 * have changed.
                 */
                if (dsp >= 0 && dsp_resolved[0] != '\0') {
                    char now[VLHE_OUT_MAX];

                    if (vlhe_resolve_out(dsp_dev, now, sizeof now) == 1
                        && strcmp(now, dsp_resolved) != 0) {
                        fprintf(stderr, "vdiscd/audio: output moved"
                                        " %s -> %s\n", dsp_resolved, now);
                        close_now(dsp, "rebind", lba);
                        dsp = -1;
                    }
                }
                if (dsp < 0) {
                    dsp = open_channel();
                    /* A level set while nothing was open - KsCD's
                     * order, VOLCTRL then PLAYMSF - lands here. */
                    if (dsp >= 0)
                        apply_volume(dsp, vol);
                }
                /*
                 * A DEVICE THAT IS NOT THERE YET IS NOT AN ERROR -
                 * design/43 part A, and this used to give up here.
                 *
                 * `said_waiting' is set only by the resolver
                 * answering "vsound is not loaded", which during a
                 * load is a state that fixes itself. So mark the
                 * track as wanted and let the idle branch retry;
                 * anything else - a busy card, a missing node - is
                 * still an error and still reported.
                 *
                 * WITHOUT THIS THE RETRY BELOW CAN NEVER RUN,
                 * because `playing' would still be 0.
                 */
                if (dsp < 0 && !said_waiting) {
                    if (report)
                        report(ctx, lba, VDISC_AUDIO_ERROR, track);
                    break;
                }
                playing   = 1;
                paused    = 0;
                wrote_any = 0;
                reported = lba;
                /* PER-PLAY, not per-session: a stall in track 7 is not
                 * made clearer by counters carrying track 2's totals. */
                logged   = lba;
                wr_calls = 0;
                wr_short = 0;
                wr_bytes = 0;
                wr_slow  = 0;
                wr_worst = 0;
                if (report)
                    report(ctx, lba, VDISC_AUDIO_PLAY, track);
                break;

            case CMD_PAUSE:
                paused = 1;
                if (report)
                    report(ctx, lba, VDISC_AUDIO_PAUSED, track);
                break;

            case CMD_RESUME:
                paused = 0;
                if (report)
                    report(ctx, lba, VDISC_AUDIO_PLAY, track);
                break;

            case CMD_VOLUME: {
                /*
                 * SET THIS CHANNEL'S VOLUME - and REMEMBER IT.
                 *
                 * Until 2026-09-14 this applied the level to the open
                 * fd and otherwise dropped it, on the reasoning that
                 * the kernel caches the value for CDROMVOLREAD so
                 * nothing observable was lost. What was lost was the
                 * level itself: the channel is closed between tracks
                 * and while stopped, and KsCD sets its volume
                 * immediately BEFORE every play (142 VOLCTRL-then-PLAY
                 * pairs in the filed traces), so the slider did nothing
                 * to the track that followed. design/26 B5.
                 */
                vol = m.arg;
                apply_volume(dsp, vol);     /* now if open, else at open */
                break;
                }

            case CMD_STOP:
                playing = 0;
                paused  = 0;
                /*
                 * CLOSE THE CHANNEL. Section 3.1a: a stopped disc must
                 * not hold one of four. The volume tool's row goes
                 * (free) here, which is correct - a slider for
                 * something silent is noise.
                 */
                if (dsp >= 0) {
                    /* STOP MEANS STOP - discard, do not play out the
                     * remaining buffer. */
                    close_now(dsp, "stop", lba);
                    dsp = -1;
                }
                if (report)
                    report(ctx, lba, VDISC_AUDIO_NO_STATUS, track);
                break;

            default:
                break;
            }
            continue;                   /* drain the pipe first */
        }

        if (!playing || paused || dsp < 0) {
            /*
             * SAY WHY WE ARE IDLE, ONCE PER STATE CHANGE.
             *
             * This gate is where a child that has been told to play
             * sits silently doing nothing, and until 2026-08-27 there
             * was NO WAY TO SEE IT. The kernel trace showed the
             * channel at `flags 0x1' - BUSY, never RUNNING - for 836
             * consecutive samples while another client held a channel,
             * then MIXING the instant that client released. Open,
             * configured, never written to.
             *
             * The user reported this from the first run of the day
             * ("Doom and the CD are fighting each other"). Six
             * theories were built and eliminated against kernel-side
             * data because THIS side had no instrumentation at all.
             *
             * Rate-limited to state CHANGES: at 20 ms per pass an
             * unconditional print would be 50 lines a second.
             */
            {
                static int last_state = -1;
                int state = (!playing ? 1 : 0)
                          | (paused   ? 2 : 0)
                          | (dsp < 0  ? 4 : 0);

                if (state != last_state) {
                    fprintf(stderr, "vdiscd/audio: idle -%s%s%s"
                                    " (lba %u, end %u)\n",
                            (!playing) ? " not-playing" : "",
                            paused     ? " paused"      : "",
                            (dsp < 0)  ? " no-dsp"      : "",
                            lba, end);
                    last_state = state;
                }
            }
            /*
             * AND IF PLAY WAS WANTED BUT THE DEVICE WAS NOT THERE,
             * TRY AGAIN - design/43 part A.
             *
             * Until 2026-09-26 a failed open at CMD_PLAY reported
             * VDISC_AUDIO_ERROR and that was the end of it: the
             * track was lost until the application sent another
             * play. That was survivable while `-o' was a fixed path
             * resolved before exec, because a failure then meant a
             * device that was not coming. With `@VSOUND@' it means
             * "vsound is not loaded YET", which is a normal state
             * during a load and resolves on its own.
             *
             * TWICE A SECOND, NOT EVERY 20 ms. The open is cheap but
             * not free, and vlhe_resolve_out() reads /proc each
             * time; the pass rate is for feeding audio, not for
             * polling. vmidid retries at 250 ms for the same reason.
             */
            if (playing && !paused && dsp < 0) {
                static long retry_ms;
                static int  said_paused;

                retry_ms += 20;
                if (retry_ms >= 500) {
                    retry_ms = 0;
                    dsp = open_channel();
                    if (dsp >= 0) {
                        apply_volume(dsp, vol);
                        fprintf(stderr, "vdiscd/audio: device arrived -"
                                        " resuming at lba %u\n", lba);
                        said_paused = 0;
                        if (report)
                            report(ctx, lba, VDISC_AUDIO_PLAY, track);
                    } else if (report) {
                        /*
                         * WAITING IS NOT STALLING - design/36 row 139,
                         * 86Box 2026-10-03. With no device (vsoundd
                         * holding the card, vsound not ready, another
                         * daemon on an exclusive card) this child
                         * sent no position, so the MODULE timed the
                         * PLAY out to ERROR after 10 s ("the audio
                         * child has stalled") - and KsCD, reading
                         * ERROR, said it ejected the disc and stopped.
                         *
                         * SO REPORT THE UNCHANGED POSITION AS PAUSED,
                         * on every retry. The module exempts PAUSED
                         * from its stall rule (vdisc_audio_sample()),
                         * a player shows "paused" rather than an error,
                         * and the success branch above reports PLAY
                         * the moment the device arrives. A Resume
                         * pressed meanwhile reports PLAY once; the
                         * next retry, 500 ms on, says PAUSED again.
                         */
                        report(ctx, lba, VDISC_AUDIO_PAUSED, track);
                        if (!said_paused) {
                            fprintf(stderr, "vdiscd/audio: no device -"
                                            " reporting PAUSED at lba %u"
                                            " until one is ready\n", lba);
                            said_paused = 1;
                        }
                    }
                }
            }
            usleep(20000);              /* 20 ms: idle, not spinning */
            continue;
        }

        /*
         * FOLLOW THE OUTPUT WHILE A TRACK PLAYS - design/54 6b step 2,
         * 2026-10-03. The output was re-resolved only at a play command
         * (2bd0622), so a track that kept playing stayed on the card
         * while vsound came up (design/36 row 137). Once a second the
         * same question is asked again; vlhe_resolve_out() says vsound
         * once vsoundd is READY, WAIT while vsoundd is waiting for this
         * very card, the card otherwise. A literal `-o' never changes.
         *
         * A NEW DEVICE is opened at once, at the same lba, after
         * close_now() discards what the old one queued; WAIT leaves
         * the channel closed and the idle branch above retries every
         * 500 ms while `playing' holds - so the track resumes where it
         * was once vsoundd has the card.
         */
        {
            static struct timeval last_follow;
            struct timeval tnow;

            gettimeofday(&tnow, (struct timezone *) 0);
            if (last_follow.tv_sec == 0
                || (tnow.tv_sec - last_follow.tv_sec) * 1000L
                   + (tnow.tv_usec - last_follow.tv_usec) / 1000L >= 1000L
                || tnow.tv_sec < last_follow.tv_sec) {
                char now[VLHE_OUT_MAX];
                int  r;

                last_follow = tnow;
                r = vlhe_resolve_out(dsp_dev, now, sizeof now);
                if (r == 0 || (r == 1 && strcmp(now, dsp_resolved) != 0)) {
                    if (r == 0)
                        fprintf(stderr, "vdiscd/audio: letting go of %s -"
                                        " vsoundd is waiting for it\n",
                                dsp_resolved);
                    else
                        fprintf(stderr, "vdiscd/audio: output moved"
                                        " %s -> %s\n", dsp_resolved, now);
                    close_now(dsp, "follow", lba);
                    dsp = -1;
                    if (r == 1) {
                        dsp = open_channel();
                        if (dsp >= 0)
                            apply_volume(dsp, vol);
                    }
                    if (dsp < 0)
                        continue;       /* the idle branch retries */
                }
            }
        }

        if (lba >= end) {               /* track finished */
            playing = 0;
            if (dsp >= 0) {
                /* THE ONE CLOSE THAT SHOULD DRAIN - the track really
                 * ended, so the queued tail is wanted. Logged anyway,
                 * so the log can tell it apart from the others. */
                fprintf(stderr, "vdiscd/audio: close (track end) at"
                                " lba %u, end %u\n", lba, end);
                close(dsp);
                dsp = -1;
            }
            /*
             * COMPLETED, not NO_STATUS. The CD-ROM API distinguishes
             * "the track ended on its own" from "someone stopped it",
             * and an application waiting for playback to finish watches
             * for exactly this.
             */
            if (report)
                report(ctx, lba, VDISC_AUDIO_COMPLETED, track);
            continue;
        }

        want = (int) (end - lba);
        if (want > CHUNK_FRAMES)
            want = CHUNK_FRAMES;

        /*
         * THE READ THAT CAN HANG. This is why the child exists: it is
         * blocking, uninterruptible I/O on a file that may be on
         * failing media. If it wedges, this process goes into D state
         * and the PARENT carries on serving block reads.
         */
        /*
         * `< 0', NOT `<= 0'. read_audio() returns ZERO ON SUCCESS - it
         * reports status, not a byte or frame count. Testing <= 0
         * treats every good read as a failure, which is exactly what
         * this file did on 2026-08-25: the child opened its channel,
         * did one perfectly good read, reported ERROR and closed. The
         * trace showed the open and release a few ticks apart with
         * `status now ERROR' thereafter, and no audio ever played.
         *
         * The old tree had it right (vcdd.c:1823, `if (rc_read < 0)').
         * The convention was inverted here when the read became a
         * callback.
         */
        n = read_frames(ctx, lba, (unsigned int) want, buf);
        if (n < 0) {
            /* Report it. A silent stop is indistinguishable from a
             * track ending, which is an ambiguity that has cost this
             * project time before. */
            fprintf(stderr, "vdiscd/audio: read failed at lba %u -"
                            " stopping\n", lba);
            if (report)
                report(ctx, lba, VDISC_AUDIO_ERROR, track);
            playing = 0;
            if (dsp >= 0) {
                close_now(dsp, "read failed", lba);
                dsp = -1;
            }
            continue;
        }

        /*
         * `want' FRAMES, not `n'. read_audio() returns 0 on success, so
         * n is a status and never a count - using it here wrote zero
         * bytes and left lba unchanged, so playback would have sat
         * still even once the error test above was right. The number of
         * frames in the buffer is what we ASKED for, which is `want'.
         */
        /* THE FIRST WRITE OF A PLAY is what starts the channel in the
         * kernel - a channel with no write is BUSY but never RUNNING,
         * and the mixer skips it. Announce it once so "the child was
         * told to play" and "the child actually wrote" stop being the
         * same line in a log. */
        if (!wrote_any) {
            wrote_any = 1;
            fprintf(stderr, "vdiscd/audio: first write, %d bytes at"
                            " lba %u\n", want * RAW_SECTOR, lba);
        }

        /*
         * TIME THE WRITE. See WR_SLOW_MS above for why: this child is
         * single-threaded, so a write that blocks is a read that does
         * not happen.
         */
        {
            struct timeval t0, t1;
            long ms;
            int asked = want * RAW_SECTOR;
            int done = 0, failed = 0, was_short = 0;

            /*
             * THE WHOLE CHUNK, HOWEVER MANY WRITES IT TAKES.
             *
             * vsound's blocking write returns SHORT when its ring has
             * been full for vsound_write_ms (20 ms) - a legal partial
             * count, and on DMA it happens on 0.8-1 % of writes, mostly
             * at track start and end (CLAUDE.md section 4). This used
             * to advance `lba' by the whole frames accepted and re-read
             * from there, so the PARTIAL frame the kernel had already
             * taken - up to 2351 bytes - was sent again from its start:
             * up to 13 ms repeated, where its own comment said "loses
             * at most one frame". design/26 B10, fixed 2026-09-14;
             * vmidid had the mirror-image bug (design/24 B1) and got
             * the same loop.
             *
             * Resume from the remainder until the chunk is gone. The
             * timing brackets the whole chunk, which is what the "worst
             * ms" figure was always meant to say.
             */
            /*
             * AND STOP WHEN TOLD TO. on_term() clears `running' AND
             * CLOSES THE CHANNEL, so that a stop releases it at once
             * (the handler's comment above). vsound answers the write
             * in progress with a short count when the signal lands;
             * the first version of this loop then resumed on the fd
             * the handler had just closed, got EBADF, and the new
             * error report below marked the stop as a play ERROR -
             * which KsCD shows as "Ejected" and will not play from.
             * Both 86Box runs of 2026-09-15 (run 77). A shutdown is
             * not a failure: the loop ends and nothing is reported.
             */
            gettimeofday(&t0, (struct timezone *) 0);
            while (done < asked && running) {
                wrote = (int) write(dsp, buf + done, (size_t) (asked - done));
                if (wrote < 0) {
                    if (errno == EINTR)
                        continue;           /* the loop test sees running */
                    failed = errno;
                    break;
                }
                if (wrote == 0) {
                    failed = EIO;           /* a device taking nothing */
                    break;
                }
                done += wrote;
                if (done < asked)
                    was_short = 1;
            }
            gettimeofday(&t1, (struct timezone *) 0);

            if (!running)
                continue;                   /* stopping: not an error */


            ms = (t1.tv_sec - t0.tv_sec) * 1000
                 + (t1.tv_usec - t0.tv_usec) / 1000;
            if (ms < 0)
                ms = 0;
            wr_calls++;
            wr_bytes += (unsigned long) done;
            if ((unsigned long) ms > wr_worst)
                wr_worst = (unsigned long) ms;
            if (ms >= WR_SLOW_MS)
                wr_slow++;
            if (was_short) {
                /* Once per chunk that needed resuming, not per resume,
                 * so the figure means "the write waited over 20 ms"
                 * and stays comparable with the filed logs. */
                wr_short++;
                if (wr_short <= 3)
                    fprintf(stderr, "vdiscd/audio: SHORT WRITE at lba %u"
                                    " - resumed, %d of %d bytes\n",
                            lba, done, asked);
            }

            if (failed) {
                /*
                 * REPORT IT, as the read-error path does. Until
                 * 2026-09-14 this closed the channel and said nothing
                 * to the kernel, whose status stayed at the last
                 * PUT_POS - PLAY - with the position frozen: a player
                 * waiting for COMPLETED waited forever, Quake's CD
                 * track never advanced, KsCD's clock stopped. The
                 * trigger is vsound's -EIO after its 1 s hard deadline
                 * with nothing accepted - the pump dead or stalled.
                 * design/26 B7.
                 */
                fprintf(stderr, "vdiscd/audio: write: %s\n",
                        strerror(failed));
                if (report)
                    report(ctx, lba, VDISC_AUDIO_ERROR, track);
                playing = 0;
                close_now(dsp, "write failed", lba);
                dsp = -1;
                continue;
            }
        }

        lba += (unsigned int) want;

        /*
         * THE POSITION, PERIODICALLY. The line this whole block exists
         * for - see the declarations above.
         */
        if (lba - logged >= LOG_FRAMES) {
            fprintf(stderr, "vdiscd/audio: lba %u (track %d), %lu writes,"
                            " %lu short, %lu KB, worst %lu ms, %lu slow\n",
                    lba, track, wr_calls, wr_short, wr_bytes / 1024,
                    wr_worst, wr_slow);
            logged = lba;
        }

        if (report && lba - reported >= REPORT_FRAMES) {
            /*
             * THE TRACK FOLLOWS THE POSITION, not the play command.
             *
             * A play may span several tracks - a CD player asking for
             * "from here to the end of the disc" is an ordinary
             * request, and the log for 2026-08-25 has
             * `play 111714..277874' from exactly that. Reporting the
             * track captured at play time leaves SUBCHNL claiming
             * track 6 while the audio is physically in track 7.
             *
             * Real hardware cannot get this wrong: it reads the track
             * number off the disc's Q sub-channel as it plays
             * (cm206.c:995, `qp->cdsc_trk = q[1]'). We have no
             * sub-channel, so we look it up in the TOC - which is the
             * same answer by a different route.
             */
            if (track_at) {
                int now = track_at(ctx, lba);

                if (now > 0) {
                    /*
                     * SAY IT. The transition was detected here for
                     * SUBCHNL's sake and left no trace in the log,
                     * so "track 10 became track 11 mid-stream" was
                     * visible only by reading two adjacent progress
                     * lines and noticing the number moved.
                     *
                     * IT MATTERS BECAUSE THE CHANNEL DOES NOT CLOSE
                     * HERE - see the rebind check in CMD_PLAY. A
                     * boundary crossed inside one play command is
                     * the case that hid a missing rebind for a
                     * whole afternoon.
                     */
                    if (now != track)
                        fprintf(stderr, "vdiscd/audio: track %d -> %d"
                                        " at lba %u (same channel)\n",
                                track, now, lba);
                    track = now;
                }
            }
            report(ctx, lba, VDISC_AUDIO_PLAY, track);
            reported = lba;
        }
    }

    /*
     * AND THIS IS THE ONE THAT WAS AUDIBLE - 2026-09-27.
     *
     * `vdiscd_play_stop()' sends CMD_STOP and then KILLS the child
     * (`vdiscd_play.c:373'), so on a stop this close often does not
     * run at all - the process dies holding the device and the
     * KERNEL closes it, which drains. Where the child does reach
     * here, discarding is what a shutdown wants.
     *
     * The kernel's own close cannot be given a RESET, so the real
     * cure for the killed case is that a stop reaches CMD_STOP
     * first; this is the backstop for an orderly exit.
     */
    if (dsp >= 0)
        close_now(dsp, "child exit", lba);
    return 0;
}
