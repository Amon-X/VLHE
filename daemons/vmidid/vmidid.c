/*
 * vmidid - the synth daemon: /dev/vmidi in, /dev/dsp out.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * THE MIDDLE OF THE CHAIN, AND THE PIECE THAT WAS MISSING.
 *
 *     lxdoom --pipe(MUS+GENMIDI)--> musserv -m -u N
 *                                     |
 *                                     v  /dev/sequencer
 *                               vmidi's MIDI device (kernel)
 *                                     |
 *                                     v  /dev/vmidi
 *                               vmidid  <-- THIS
 *                                     |
 *                                     v  /dev/dsp
 *                                   vsound  ->  the card
 *
 * Everything either side of it existed before this file did.
 * `smf2wav' renders a MIDI FILE to a WAV on the workstation, which is
 * the development loop; this renders a LIVE STREAM to the card, which
 * is what the project is for. The renderer is the same code - render.c
 * knows nothing about where its events came from.
 *
 * WHAT IS DIFFERENT ABOUT A LIVE STREAM, and it is the whole design:
 *
 *   - THE EVENTS ARRIVE AS BYTES, not as a parsed file with delta
 *     times. Running status, and messages split across reads.
 *   - THERE IS NO END. smf2wav renders until the events run out;
 *     this runs until it is killed, and must handle a stream that
 *     stops without finishing (render_panic - see the idle section).
 *   - THE DEADLINE IS REAL. smf2wav renders as fast as it can with
 *     nobody waiting. Here the card consumes at exactly the sample
 *     rate and a block late is a block of silence.
 *
 * C89, ASCII only, cross-built with the target's GCC 2.95.2.
 */

/*
 * POSIX, REQUESTED EXPLICITLY - the same reason vmidicat.c does it:
 * sigaction and struct sigaction live behind __USE_POSIX in this
 * libc's signal.h, which `gcc -ansi' does not define on its own.
 * Without it the C89 conformance check reports them as implicit
 * declarations - it still LINKS, because the symbols exist, but an
 * implicit int-returning declaration of a function taking pointers is
 * exactly the class of thing that works until it does not.
 */
#define _POSIX_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include "vlhe_status.h"
#include "vmidid_ctl.h"
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include "vsound.h"      /* VSOUND_IOC_STAT - is this vsound? */

#include "sf2.h"
#include "sf2gen.h"
#include "render.h"
#include "autovoice.h"

/*
 * THE DAEMON'S VOICE CAP - 512 since 2026-10-02 (the user: "cap at 512
 * default to 64"). The renderer allocates only what the ceiling asks
 * for (design/21 section 17), so the default stays RENDER_DEFAULT_VOICES
 * (64, ~420 KB of state as before) and -p 512 costs its own memory
 * only when asked. Whether a machine can RENDER 512 in real time is a
 * separate question, and automatic voice reduction answers it.
 */
#ifndef VMIDID_MAX_VOICES
#define VMIDID_MAX_VOICES 512
#endif
/* `make MAX_VOICES=' sets it (defs.mk). The default must fit under it,
 * and the renderer cannot go past its own bound. */
#if VMIDID_MAX_VOICES < RENDER_DEFAULT_VOICES
#error "MAX_VOICES is below the default of 64 voices"
#endif
#if VMIDID_MAX_VOICES > RENDER_MAX_VOICES
#error "MAX_VOICES is above the renderer's bound, RENDER_MAX_VOICES (2048)"
#endif


#define MIDI_DEV_DEFAULT "/dev/vmidi"
#define DSP_DEV_DEFAULT  "/dev/dsp"

/*
 * WHERE A SOUNDFONT IS EXPECTED TO BE INSTALLED. Named for the error
 * message only - THERE IS NO DEFAULT and -s is required, because a
 * daemon that silently picks up a font from a path the user did not
 * name is a daemon whose output nobody can attribute. design/09 has
 * this path in the fresh-install section, and the .conf files carry
 * it for load.sh so it lives in one place rather than being typed in.
 */
#define SF2_INSTALL_PATH "/usr/local/share/sounds/sf2"

#define SYNTH_RATE     44100
#define SYNTH_CHANNELS 2

/*
 * BLOCK SIZE AND QUEUE DEPTH ARE RUNTIME PARAMETERS, NOT CONSTANTS.
 *
 * THE USER'S CALL, 2026-09-07, and the reasoning is worth keeping
 * because it is the opposite of what this file first proposed.
 *
 * design/p3-midi-plan.md sizes a 16-block render-ahead queue from
 * measurements taken on the ACER, before the float filter and the
 * stereo-zone work, with a 1024-frame block that its own text says
 * "was picked for a file renderer with no one waiting". 1024 frames
 * is 23.2 ms, and sixteen of them is 370 ms of MIDI-to-sound latency,
 * which is a great deal for a game.
 *
 * The obvious move is to pick a smaller number here. THAT WOULD JUST
 * REPLACE ONE GUESSED NUMBER WITH ANOTHER - and this project has been
 * wrong repeatedly about figures reasoned rather than measured: the
 * modulator layer's "75%" was out by 8.6x, the "233 MHz Pentium II"
 * was never the target, and the Acer's disk ran at 3.5 MB/s for weeks
 * because a config option was assumed rather than checked.
 *
 * So both are -b and -q, and the P3 decides. The defaults below are a
 * STARTING POINT FOR MEASUREMENT, not an answer:
 *
 *     256 frames x 8 deep = 5.8 ms x 8 = 46 ms
 *
 * AND ONLY THE P3 CAN ANSWER IT. 86Box cannot show starvation at all
 * - it slows CPU, disk and timers together, so a producer never gets
 * far enough ahead of a consumer to starve it (CLAUDE.md section 4).
 * A run there smoke-tests the plumbing and says nothing about depth.
 *
 * ENABLE DMA FIRST: a PIO run measures the disk, not this code.
 *
 * THE QUEUE IS OFF BY DEFAULT SINCE 2026-09-14 - design/24 P4, and
 * the runs of that day. The render-ahead never protected anything:
 * the loop refilled the queue before every write, so the write of
 * block N was always preceded by the render of block N+q-1, and a
 * slow render delayed the write by the same amount however many
 * blocks sat behind it. The only audio downstream of a render when
 * it starts is vsound's ring, whatever -q says. What the queue DID
 * do was add q-1 blocks of MIDI-to-sound latency (40 ms at 256 x 8)
 * and a 7 KB memmove per block. Seen live: 86Box run 213317 ran the
 * ring dry eight times with "8 deep (185 ms)" in its banner, and the
 * Acer's 214434 and 215016 starved with the same queue full.
 *
 * So 0 means render a block and write it, and -q N keeps the old
 * queue so the two can be compared on a target - the user's request,
 * one audible change at a time. The plan's real requirement, audio
 * queued ahead of the card against the synth being descheduled, would
 * have to live in the kernel ring to count; the user does not want
 * that delay, and design/07's scheduling-priority section is the
 * no-delay answer if it is ever needed.
 */
#define BLOCK_FRAMES_DEFAULT 256
#define QUEUE_BLOCKS_DEFAULT 0

#define BLOCK_FRAMES_MAX 8192
#define QUEUE_BLOCKS_MAX 64

/* The exit drain's length, in blocks. It used to borrow -q, which is
 * now 0 by default: 8 blocks is 46 ms at the default block, the tail
 * that was already committed under the old queue. */
#define DRAIN_BLOCKS 8

/*
 * THE IDLE PANIC WINDOW - how long the stream must be quiet, with
 * voices still sounding, before render_panic() runs.
 *
 * See the note above check_idle(). 2 seconds by default: long enough
 * that no musical rest reaches it, short enough that a killed client
 * does not drone for an audible age.
 */
#define IDLE_PANIC_MS_DEFAULT 2000

/*
 * RELEASE THE CHANNEL AFTER THIS LONG WITH NO VOICE SOUNDING.
 *
 * WHY THE DAEMON LETS GO OF /dev/dsp AT ALL. The blocking write is
 * the timebase, so a resident vmidid writes a block every 5.8 ms
 * whether or not anything is sounding - and to vsound a stream of
 * zeros is indistinguishable from music. A pump running -R releases
 * the card when the last client closes, which is how the Acer avoids
 * ITS hanging note - esssolo1 ON THAT MACHINE, and see the note at
 * the end: the card and the laptop cannot be separated. A synth that
 * never closes means the card
 * is never idle and -R never fires. So the synth closes the channel
 * once it has been silent for a while, and reopens on the next MIDI
 * byte. Nothing else opens with O_EXCL, so the reserved slot cannot
 * be taken in between (vsound_chan.h, VSOUND_MIDI_SLOT).
 *
 * MEASURED 2026-09-21 ON THE ACER, AND HALF OF THE ABOVE DID NOT
 * HAPPEN. The two claims in that sentence were tested separately:
 *
 *   "a pump running -R ... avoids the hanging note"
 *       CONFIRMED, both directions. Without -R on the pump a tone
 *       sounds through the teardown and stops the instant vsoundd
 *       does; with -R there is nothing. vsoundd.c:469-478 has why -
 *       only close() runs the driver's drain_dac()/stop_dac().
 *
 *       BUT THE TEARDOWN IS NOT WHEN IT STARTS - 2026-09-29. The
 *       tone is audible from LOAD and audio MASKS it; the pump is
 *       itself a writer, so it is covered until the writers stop,
 *       which during teardown is the window described above. One
 *       tone, two moments. design/16 section 1a-i reconciles this
 *       with section 1a, which says load and which this comment
 *       appeared to contradict.
 *
 *   "a synth that never closes means ... -R never fires"
 *       THE TRIGGER, YES - vsound_dev.c:1228 gates on nbusy == 0 and
 *       a held channel keeps it at 1. THE CONSEQUENCE, NO: with
 *       `-R 0' here and -R on the pump, Doom played and quit on the
 *       Acer with NO HANGING NOTE.
 *
 * So the release still earns its place, and the specific harm this
 * paragraph predicts from holding the channel is UNPROVEN on the one
 * machine it names. Three explanations fit and this test separates
 * none of them: the note may need more than an unreleased card, a
 * later fix may have removed it (c54fd4d, 27c9a28 and 6ac4317 all
 * touched these paths in September), or the attribution may have
 * been reasoned rather than measured.
 *
 * tests/logs/2026-09-21-acer-release-flag-matrix/ has the matrix.
 * DO NOT TIGHTEN OR REMOVE THE RELEASE ON THE STRENGTH OF THIS -
 * finding 1 stands, and it is the reason the default is not 0.
 *
 * AND IT IS `esssolo1 ON THE ACER', NOT EITHER HALF ALONE. The
 * user's elimination, 2026-09-22: five machines have run this
 * software and only one hangs.
 *
 *     Soyo / Deluxe      es1371      no
 *     the previous board es1371      no
 *     the P3             EMU10K1     no
 *     86Box              SB16        no
 *     THE ACER           esssolo1    YES
 *
 * The card is in a LAPTOP and cannot be moved, so the driver cannot
 * be separated from the board. A PCI Solo-1 costs $120-300 and would
 * not answer it either - a different board and revision tests only
 * that combination. NOBODY SHOULD BUY ONE.
 *
 * THE TEST THAT WOULD SEPARATE IT IS ALREADY OWNED: a second
 * MATCHING Acer, bought for spares, which boots. A fresh install on
 * it (CF card through an adapter) says whether the hang belongs to
 * the MODEL or to this UNIT - and either answer is worth having,
 * since "one laptop's quirk" has never been considered.
 *
 * NOTE WHAT THIS DOES NOT CHANGE: the shipping decision. `-R' is the
 * default, it costs nothing on a machine that does not need it, and
 * it fixes the one that does. Why it hangs is not on the path to any
 * release.
 *
 * WHAT THE REOPEN COSTS: the pump has to reacquire the card, which
 * design/16 measured at ~100 ms, so the first note after a release is
 * that late. Inside a piece that would be audible, so the threshold
 * has to be longer than any rest in which NOTHING sounds - release
 * tails included, and the SC-55's run to 3.6 s. Three seconds of
 * total silence is between songs, not between phrases. `-R' sets it;
 * 0 holds the channel for life, which is what the daemon did before.
 *
 * Measured rather than believed: if a piece has a longer true rest,
 * the per-second line shows the release and the next note's delay.
 *
 * AND THE EFFECTS' TAIL IS NOT A VOICE. `last_sound' moves only while
 * render_active() > 0; the reverb keeps sounding after the last voice
 * has ended - 1.4 s to silence on a dull tone, 0.9 s on a bright one,
 * measured 2026-09-08 (design/21 section 13) - and nothing here
 * counts it. At 3000 ms the tail is long gone before the release
 * fires, so it is invisible today. TIGHTEN THIS BELOW ABOUT 2000 AND
 * THE RELEASE CUTS THE REVERB OFF MID-DECAY, on every song's last
 * chord, and the queue drop below throws the rest away. The user
 * intends to tighten it (2026-09-08); when that happens, the fix is
 * to make the release wait for the effects as well as the voices -
 * either an effects-quiet test in the renderer (the reverb decays to
 * exact zero, so "every comb buffer reads zero" is a true test), or
 * simply adding the tail to the threshold. smf2wav hit exactly this
 * and now runs 2 s past the last voice with effects on (its `tail').
 *
 * DONE, 2026-10-03, the first way: `last_sound' now also moves while
 * the block about to be written has any non-zero sample (pcm_silent()
 * below), which the effects' exact decay to zero makes a true test -
 * so a tighter -R waits for the tail. The user asked for the key
 * (`[Midi Settings] ChannelRelease') with this guard.
 */
#define RELEASE_MS_DEFAULT 3000

/* --- the MIDI byte parser ------------------------------------------- */

/*
 * RUNNING STATUS IS THE COMMON CASE, NOT A CORNER ONE.
 *
 * A sequencer emitting a stream of note-ons on one channel sends the
 * status byte ONCE and then pairs of data bytes forever. Any parser
 * that expects three bytes per message decodes the second note as
 * garbage - and it will do so silently, producing music that is wrong
 * rather than an error that is visible.
 *
 * THE STATE THAT MUST SURVIVE A read(): a message can be split across
 * reads at any byte, so `status' and the partial data live here
 * between calls rather than on the stack of the read loop. That is
 * the bug a parser written inside the loop always has, and it only
 * shows up under load, when reads start returning partial messages.
 */
typedef struct {
    unsigned char status;      /* the running status, 0 if none */
    unsigned char data[2];
    int           ndata;       /* data bytes collected so far   */
    int           want;        /* data bytes this status needs  */
    int           in_sysex;
    /*
     * THE SYSEX BODY, collected between F0 and F7 and handed to
     * render_sysex() whole - design/22 step 2. 256 bytes holds every
     * message the renderer acts on (the longest, GS DT1, is 11) with
     * room for the parameter dumps it ignores; a longer one is
     * counted in sysex_dropped and not delivered, and a body ended
     * by a status byte rather than F7 is dropped the same way.
     */
    unsigned char sysex[256];
    int           sysex_len;
    int           sysex_over;

    unsigned long messages;
    unsigned long sysex_bytes;
    unsigned long sysex_msgs;
    unsigned long sysex_dropped;
    unsigned long ignored;
} midi_parser;

/* THE PARSER IS FOR THE DAEMON AND tests/parsecheck.c. tests/writecheck.c
 * includes this file for write_block() alone, and an unused static is a
 * gcc 2.95.2 warning - so WRITECHECK leaves these two out. */
#ifndef WRITECHECK
static void
parser_init(midi_parser *p)
{
    memset(p, 0, sizeof *p);
}

/*
 * HOW MANY DATA BYTES A STATUS TAKES. Program change and channel
 * pressure take one; everything else in the channel-voice range takes
 * two. Getting this wrong desynchronises the whole stream from that
 * point on, which is why it is a function rather than an inline guess.
 */
static int
data_bytes_for(unsigned char status)
{
    switch (status & 0xf0) {
    case 0xc0:                  /* program change   */
    case 0xd0:                  /* channel pressure */
        return 1;
    case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0:
        return 2;
    default:
        return 0;
    }
}

/*
 * Feed one byte. Complete channel-voice messages go to the renderer.
 *
 * SYSTEM REAL-TIME BYTES (0xf8..0xff) MUST NOT DISTURB THE PARSE.
 * The spec allows them to appear ANYWHERE, including between the two
 * data bytes of a note-on, and they do: seq_reset() sends 0xfe
 * (active sensing) before its all-notes-off sweep
 * (sequencer.c:1310). A parser that lets one clear the running status
 * or land in a data slot corrupts the message it interrupted.
 *
 * That is not hypothetical here - usb-midi hit exactly this class of
 * bug with 0xf5, where one stray byte corrupted every message after
 * it (fixed in 9f65f49). Real-time bytes are handled and returned
 * from before any state is touched.
 */
static void
parser_byte(midi_parser *p, unsigned char b, render_state *r)
{
    if (b >= 0xf8) {
        /* System real-time: no data bytes, no effect on running
         * status. Nothing here acts on them - clock and start/stop
         * belong to a sequencer, and we are not one. */
        p->ignored++;
        return;
    }

    if (p->in_sysex) {
        p->sysex_bytes++;
        if (b == 0xf7) {        /* end of exclusive: deliver the body */
            p->in_sysex = 0;
            if (p->sysex_over) {
                p->sysex_dropped++;
            } else {
                p->sysex_msgs++;
                render_sysex(r, p->sysex, p->sysex_len);
            }
            return;
        } else if (b >= 0x80) {
            /*
             * A STATUS BYTE ENDS A SYSEX THAT WAS NEVER TERMINATED.
             * The spec permits it and a truncated stream produces it;
             * without this the parser stays in sysex for ever and the
             * music stops with no error anywhere. The body is dropped,
             * not delivered: it was not a whole message.
             */
            p->in_sysex = 0;
            p->sysex_dropped++;
            /* fall through to handle b as the status it is */
        } else {
            if (p->sysex_len < (int) sizeof p->sysex)
                p->sysex[p->sysex_len++] = b;
            else
                p->sysex_over = 1;
            return;
        }
    }

    if (b >= 0x80) {
        if (b == 0xf0) {
            p->in_sysex   = 1;
            p->sysex_len  = 0;
            p->sysex_over = 0;
            p->status = 0;      /* sysex CLEARS running status */
            p->ndata = 0;
            return;
        }
        if (b >= 0xf0) {
            /*
             * SYSTEM COMMON (0xf1..0xf7). These clear running status
             * per the spec. We consume no data bytes for them: the
             * only ones with data are song-position and song-select,
             * which a MUS-derived stream does not produce, and
             * guessing at their length would desynchronise a stream
             * that did.
             */
            p->status = 0;
            p->ndata = 0;
            p->ignored++;
            return;
        }
        p->status = b;
        p->want   = data_bytes_for(b);
        p->ndata  = 0;
        return;
    }

    /* A data byte with no status: nothing to attach it to. */
    if (p->status == 0) {
        p->ignored++;
        return;
    }

    p->data[p->ndata++] = b;
    if (p->ndata < p->want)
        return;

    render_event(r, p->status, p->data[0],
                 p->want > 1 ? p->data[1] : 0);
    p->messages++;
    p->ndata = 0;
    /* THE STATUS STAYS SET. That is running status: the next pair of
     * data bytes is another message of the same kind. */
}
#endif /* !WRITECHECK */

/* The daemon and write_block() read it; parsecheck has neither. */
#if !defined(PARSECHECK) || defined(WRITECHECK)
static volatile int stop_now;    /* set from the signal handler only */
#endif

/*
 * VOICE CEILING NUDGES, from SIGUSR1 and SIGUSR2.
 *
 * WHY A SIGNAL AND NOT A RESTART. The user, 2026-09-19: hitting the
 * voice limit "a few times and having to reload to lower is
 * annoying" - and a reload drops the channel, so the pump reacquires
 * the card (~100 ms, design/16) and the piece stops. A signal changes
 * the ceiling between blocks with nothing interrupted.
 *
 * ONE STEP PER SIGNAL, NOT A VALUE, because a signal carries none.
 * That is the honest limit of this mechanism and the reason a config
 * reload is still wanted later - see the design note in design/21.
 *
 * `volatile sig_atomic_t' rather than int: the handler writes it and
 * the loop reads it, which is exactly the case the type exists for.
 * The handler does nothing else - no render calls, no printf - since
 * async-signal-safety allows almost nothing and the loop is checking
 * this within a block anyway.
 *
 * Only the daemon proper reads it, so the test builds that include this
 * file with PARSECHECK leave it out - gcc 2.95.2 warns "defined but not
 * used" otherwise, and any 2.95.2 warning is a defect.
 */
#ifndef PARSECHECK
static volatile sig_atomic_t voice_nudge;   /* +1 up, -1 down, 0 idle */
#endif

/*
 * WRITE A WHOLE BLOCK, RESUMING AFTER A SHORT RETURN.
 *
 * design/24 section 3, B1 - and fixed 2026-09-14. vsound's blocking
 * write returns SHORT when its ring has been full for vsound_write_ms
 * (20 ms, two jiffies): the deadline bounds each sleep so i_sem is not
 * held for long, and a partial count is the legal answer OSS has
 * always allowed. The old code compared the return to the block size,
 * counted a mismatch, and then SHIFTED THE QUEUE A WHOLE BLOCK anyway
 * - so 496 to 1008 of the 1024 bytes were never written: 2.8 to 5.7 ms
 * of music skipped, with a discontinuity at the join, 1 to 76 times a
 * run on every machine (the "short writes" figure in every filed
 * DAEMON.LOG, which said the opposite of what it counted).
 *
 * So: write, and if the driver took less, write the rest from where it
 * stopped, until the block is gone. That is what tonetest and every
 * OSS client in the reference trees do, and what design/25 section 11
 * says the kernel side expects of a client.
 *
 *   EINTR    retried, unless the signal was the stop: then -1 with
 *            errno EINTR so the caller's loop can see stop_now. What
 *            was already written stays written; the rest of the block
 *            is lost at exit, as it always was.
 *   0        a device that accepts nothing is wedged: -1, errno EIO.
 *            vsound never returns 0 on a blocking fd - it returns the
 *            short count if anything was taken and -EIO at its hard
 *            deadline if nothing ever was - so this is for any other
 *            driver.
 *   error    -1, errno as the driver left it.
 *
 * `*shorts' is raised by ONE if any write in the block came back
 * short - once per block, not per resume - so the exit line's "short
 * writes" keeps meaning "blocks whose write waited over 20 ms", and
 * stays comparable with the logs filed before this fix. It is the
 * useful fact: the daemon ran ahead of the mixer and had to wait.
 *
 * Returns 0 when every byte is written, -1 otherwise.
 *
 * Built for the daemon and for tests/writecheck.c, which defines
 * WRITECHECK beside PARSECHECK; tests/parsecheck.c has no use for it,
 * and an unused static is a gcc 2.95.2 warning.
 */
#if !defined(PARSECHECK) || defined(WRITECHECK)
static int
write_block(int fd, const void *buf, size_t len, unsigned long *shorts)
{
    const unsigned char *p = (const unsigned char *) buf;
    size_t done = 0;
    int was_short = 0;

    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);

        if (w < 0) {
            if (errno == EINTR && !stop_now)
                continue;
            break;
        }
        if (w == 0) {
            errno = EIO;
            break;
        }
        done += (size_t) w;
        if (done < len)
            was_short = 1;
    }

    if (was_short && shorts != NULL)
        (*shorts)++;
    return done == len ? 0 : -1;
}
#endif /* !PARSECHECK || WRITECHECK */

/*
 * EVERYTHING BELOW IS THE DAEMON PROPER - the sample pool, the
 * devices, and main(). tests/parsecheck.c includes THIS FILE as
 * source with PARSECHECK defined so it exercises the real parser
 * above rather than a copy of it, and a copy is what drifts. It has
 * no use for any of this and no /dev/dsp to open. tests/writecheck.c
 * does the same for write_block() above, with write() stubbed.
 */
#ifndef PARSECHECK

/*
 * IS THIS BLOCK ALL ZEROS? The release guard above the write - see
 * there. 256 stereo frames is 512 compares a block, nothing beside
 * rendering them, and it stops at the first non-zero.
 */
static int
pcm_silent(const short *pcm, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (pcm[i] != 0)
            return 0;
    return 1;
}

/* SET THE CEILING WITHIN THE DAEMON'S CAP - every path goes through
 * here: -p, the control channel, the nudges. Returns what was set. */
static int
vmidid_set_voices(render_state *r, int n)
{
    if (n > VMIDID_MAX_VOICES)
        n = VMIDID_MAX_VOICES;
    return render_set_max_voices(r, n);
}

/*
 * A RATE ASKED FOR ON THE CONTROL CHANNEL, WAITING FOR A GAP.
 *
 * 0 means nothing pending. The rate cannot be applied the moment it
 * arrives: render_set_rate() re-derives the reverb and chorus delay
 * lines, which discards their tails, and every sounding voice holds
 * a step computed from the old rate. So it lands at the next
 * RELEASE, where render_active() is already 0 and the tail is
 * three seconds gone.
 *
 * THE USER'S DECISION, 2026-09-26: *"if a user wants to change the
 * rate of vmidid that can wait until after the current stuff happens
 * when vmidid releases the card then i can apply the rate then. Or a
 * restart right away but that kills what is currently playing
 * anyways."* So deferring is not a limitation being worked around -
 * it is strictly better than the alternative, and design/43 section
 * 6b3 records that the GUI must SAY SO, because a rate change is
 * inaudible until the music stops and nothing else distinguishes
 * "waiting" from "did not work".
 */
static int pending_rate;

/* --- the control channel --------------------------------------------- */

/*
 * ONE REQUEST FROM THE CHANNEL - design/43 section 6b.
 *
 * `set <name> <value>' for the six live settings, `get <name>', and
 * `ping'. One line in, one line out, and the reply says what
 * actually happened rather than echoing the request: a caller that
 * asked for 200 voices is told it got 128, and one that asked for a
 * rate is told it is deferred.
 *
 * WHY THE REPLY MATTERS MORE THAN USUAL. Every setting here lands at
 * a different moment - gain between blocks, law at the next note,
 * the rate at the next release - so "accepted" and "in force" are
 * not the same thing and only the daemon knows which. design/43
 * 6b3 has the table and the note that the GUI must show it.
 *
 * THE RENDERER IS PASSED IN rather than reached through a global,
 * because everything else in this file takes it that way and a
 * second convention would be worse than the parameter.
 */
/* Not a velocity-filter value: SF2_MOD_VF_MASK is 0x0003, so this
 * cannot collide with one. */
#define VF_BAD 0xffffu

/* AUTOMATIC VOICE REDUCTION'S STATE - design/21 section 16. File
 * scope because the control channel changes it as well as the loop. */
static autovoice g_av;

static void
ctl_command(const char *line, render_state *r, int ctl_fd,
            int *voices_out)
{
    char verb[32], name[32];
    char reply[VMIDID_CTL_LINE];
    long val;
    int  n;

    reply[0] = '\0';

    /*
     * `set autovoice name=value,...' - THE SIX TUNABLES, ALL AT ONCE
     * (autovoice_set() says why at once). Before the general parse,
     * whose third field is a number. The spec is bounded by the line
     * itself (VMIDID_CTL_LINE) and parsed without copying.
     */
    if (strncmp(line, "set autovoice ", 14) == 0) {
        const char *why = autovoice_parse(&g_av, line + 14);
        int v[AUTOVOICE_NSET];

        autovoice_get(&g_av, v);
        if (why != NULL)
            sprintf(reply, "err autovoice %.60s", why);
        else
            sprintf(reply, "ok autovoice drain=%d,emergency=%d,healthy=%d,"
                    "settle=%d,floor=%d,tails=%d",
                    v[0], v[1], v[2], v[3], v[4], v[5]);
        if (why == NULL)
            fprintf(stderr, "vmidid: automatic voices set: %s\n",
                    reply + 3);
        vmidid_ctl_reply(ctl_fd, reply);
        return;
    }

    /*
     * sscanf WITH WIDTHS, not strtok: the line is untrusted input
     * from a FIFO anyone with the group can write, and a width is
     * the difference between a parse and an overflow.
     */
    n = sscanf(line, "%31s %31s %ld", verb, name, &val);

    if (n >= 1 && strcmp(verb, "ping") == 0) {
        sprintf(reply, "ok ping");
    } else if (n == 3 && strcmp(verb, "set") == 0) {
        if (strcmp(name, "voices") == 0) {
            int got = vmidid_set_voices(r, (int) val);

            /* THE CEILING, NOT A HAND-SET VALUE. This is what the
             * control centre's Apply sends - "match the saved
             * settings" - so it restarts automatic reduction from the
             * new -p rather than ending it; only the SIGUSR nudges do
             * that. */
            autovoice_restart(&g_av, g_av.on, got);
            *voices_out = got;
            sprintf(reply, "ok voices %d", got);
        } else if (strcmp(name, "autovoices") == 0) {
            /* `set autovoices 1|0' - the -A switch, live. Numeric, as
             * sscanf above reads the third field as a number. */
            /* FROM THE CEILING, not from wherever an earlier cut left
             * it, so switching it on again starts at -p. Apply sends
             * `set voices' first, which has set that ceiling. */
            autovoice_restart(&g_av, val ? 1 : 0, g_av.ceiling);
            *voices_out = vmidid_set_voices(r, g_av.ceiling);
            sprintf(reply, "ok autovoices %s", g_av.on ? "on" : "off");
        } else if (strcmp(name, "gain") == 0) {
            /* IN 1/1024ths, like -g after its multiply. The caller
             * sends the same units the config stores in millis, so
             * the conversion is here rather than in three callers:
             * 1000 milli = 1.0 = 1024. */
            long g = (val * 1024L) / 1000L;

            render_set_master(r, g);
            sprintf(reply, "ok gain %ld", val);
        } else if (strcmp(name, "effects") == 0) {
            /* NUMERIC: 0 off, anything else BOTH - what it meant before
             * the split, so an old caller's `set effects 1' is unchanged.
             * One effect alone is a word (below). */
            render_set_effects(r, val ? 1 : 0);
            sprintf(reply, "ok effects %s", val ? "on" : "off");
        } else if (strcmp(name, "bypass") == 0) {
            render_set_filter_bypass(r, val ? 1 : 0);
            sprintf(reply, "ok bypass %s", val ? "on" : "off");
        } else if (strcmp(name, "modenv") == 0) {
            /* design/54 D28: 1 fast, 0 reference. Between blocks is
             * safe - a switch to reference catches up on the next. */
            render_set_menv_mode(r, val ? RENDER_MENV_FAST
                                        : RENDER_MENV_REFERENCE);
            sprintf(reply, "ok modenv %s", val ? "fast" : "reference");
        } else if (strcmp(name, "rate") == 0) {
            /*
             * DEFERRED, AND SAID SO. Refusing an out-of-range rate
             * HERE rather than at the release means the caller
             * learns immediately, when it can still show a message
             * against the control the user just moved.
             */
            if (val < RENDER_RATE_MIN || val > RENDER_RATE_MAX) {
                sprintf(reply, "err rate %ld out of range %d..%d",
                        val, RENDER_RATE_MIN, RENDER_RATE_MAX);
            } else {
                pending_rate = (int) val;
                sprintf(reply, "ok rate %ld deferred", val);
            }
        } else {
            sprintf(reply, "err unknown setting %.20s", name);
        }
    } else if (n == 2 && strcmp(verb, "set") == 0) {
        /*
         * THE TWO WORD-VALUED SETTINGS, and they are `set' with a
         * non-numeric third field - which sscanf's %ld rejected, so
         * n came back 2. Re-read the tail as a word.
         */
        char word[32];

        if (sscanf(line, "%31s %31s %31s", verb, name, word) == 3) {
            if (strcmp(name, "law") == 0) {
                unsigned law = (strcmp(word, "linear") == 0)
                             ? SF2_MOD_LAW_LINEAR : SF2_MOD_LAW_SPEC;

                /* THE VELOCITY FILTER IS IN THE SAME WORD, so the
                 * law must be OR'd into what is already there
                 * rather than replacing it - see -L and -F. */
                render_set_velfilter(r, (r->modflags & ~SF2_MOD_LAW_MASK)
                                        | law);
                sprintf(reply, "ok law %s",
                        law == SF2_MOD_LAW_LINEAR ? "linear" : "spec");
            } else if (strcmp(name, "velfilter") == 0) {
                unsigned vf;

                if (strcmp(word, "awe") == 0)       vf = SF2_MOD_VF_AWE;
                else if (strcmp(word, "201") == 0)  vf = SF2_MOD_VF_201;
                else if (strcmp(word, "204") == 0)  vf = SF2_MOD_VF_204;
                else if (strcmp(word, "none") == 0) vf = SF2_MOD_VF_NONE;
                else                                vf = VF_BAD;

                /* NO EARLY RETURN - it would skip the one exit
                 * below, and with it the log line. A rejected value
                 * must be recorded exactly as an accepted one is. */
                if (vf == VF_BAD) {
                    sprintf(reply, "err velfilter %.20s", word);
                } else {
                    render_set_velfilter(r, (r->modflags & ~SF2_MOD_VF_MASK)
                                            | vf);
                    sprintf(reply, "ok velfilter %s", word);
                }
            } else if (strcmp(name, "modenv") == 0) {
                if (strcmp(word, "fast") == 0) {
                    render_set_menv_mode(r, RENDER_MENV_FAST);
                    sprintf(reply, "ok modenv fast");
                } else if (strcmp(word, "reference") == 0) {
                    render_set_menv_mode(r, RENDER_MENV_REFERENCE);
                    sprintf(reply, "ok modenv reference");
                } else {
                    sprintf(reply, "err modenv %.20s", word);
                }
            } else if (strcmp(name, "effects") == 0) {
                /* THE -E WORDS - on, off, reverb, chorus (2026-10-05).
                 * Anything else is refused rather than read as off, as
                 * the old `on'-or-not test did. */
                int fx = render_fx_parse(word);

                if (fx < 0) {
                    sprintf(reply, "err effects %.20s", word);
                } else {
                    render_set_fx(r, fx);
                    sprintf(reply, "ok effects %s", render_fx_name(fx));
                }
            } else if (strcmp(name, "bypass") == 0) {
                /* `on'/`off' as words, since that is what the
                 * command line takes and a human would type. */
                int on = (strcmp(word, "on") == 0);

                render_set_filter_bypass(r, on);
                sprintf(reply, "ok %s %s", name, on ? "on" : "off");
            } else {
                sprintf(reply, "err unknown setting %.20s", name);
            }
        } else {
            sprintf(reply, "err malformed");
        }
    } else if (n >= 2 && strcmp(verb, "get") == 0) {
        if (strcmp(name, "rate") == 0)
            sprintf(reply, "ok rate %d%s", r->rate,
                    pending_rate ? " (a change is pending)" : "");
        else if (strcmp(name, "voices") == 0)
            sprintf(reply, "ok voices %d", r->max_voices);
        else
            sprintf(reply, "err unknown setting %.20s", name);
    } else {
        sprintf(reply, "err malformed");
    }

    /*
     * SAY IT, UNCONDITIONALLY - the user's call, 2026-09-26, and it
     * matches what the rest of this file does: only 8 of its 68
     * messages sit behind -v, and those are the per-second
     * counters. A settings change is a FACT ABOUT THE RUN, like the
     * not-vsound warning and `output moved' elsewhere in this file.
     *
     * WHY IT IS NOT MERELY FOR TESTING, though that is what
     * prompted it. Confirming the Apply settings button worked was
     * the immediate need - a request accepted while the synth is
     * idle leaves no trace until the next release, so a success and
     * a failure look identical in the log. The LASTING reason is
     * that the summary counters now describe a MOVING TARGET: a run
     * whose voice cap went 64 -> 1 -> 13 reports one aggregate
     * (4997 of 10221 notes stolen, 2026-09-26) which means nothing
     * without knowing when each setting was in force. This line is
     * that timeline.
     *
     * THE PARSED FIELDS, NEVER THE RAW LINE. `reply' is built from
     * %31s-bounded scans and our own format strings; `line' came
     * off a FIFO anyone in the group can write, and this text ends
     * up in a log we read and paste into documents.
     *
     * ONE EXIT, so no branch can skip it - the velfilter error
     * above used to return early and now falls through here.
     */
    fprintf(stderr, "vmidid: ctl: %s\n", reply);

    vmidid_ctl_reply(ctl_fd, reply);
}


static void
on_signal(int sig)
{
    (void) sig;
    stop_now = 1;
}

/*
 * SIGUSR1 lowers the voice ceiling, SIGUSR2 raises it.
 *
 * ACCUMULATES rather than overwrites, so two quick SIGUSR1s move two
 * steps even if the loop has not looked in between - dropping one
 * would make repeated signalling unreliable in exactly the case
 * someone is spamming the key because the machine is struggling.
 */
static void
on_voice_signal(int sig)
{
    if (sig == SIGUSR1) {
        if (voice_nudge > -64)
            voice_nudge--;
    } else {
        if (voice_nudge < 64)
            voice_nudge++;
    }
}

/* --- the sample pool ------------------------------------------------ */

/*
 * MALLOC'D, AND THAT IS A DECISION RATHER THAN AN OVERSIGHT.
 *
 * smf2wav's load_pool has the same code and a comment saying the
 * target "will have to be mmap'd or streamed". THE NUMBER BEHIND THAT
 * WORRY WAS WRONG, corrected by the user 2026-09-07 and worth writing
 * down because it was wrong twice:
 *
 *   - the "Memory: 64104k" line in the P3's own dmesg is a
 *     CONSTRAINED BOOT, not the machine's hardware;
 *   - the 64 MB at p3-midi-plan.md:1199 is the DreamBlaster X2's
 *     soundbank FLASH - a wavetable daughterboard on a MIDI cable,
 *     not system RAM and not in this synth's data path at all.
 *
 * THE P3 HAS 512 MB. The SC-55's 9.8 MB is trivial and even
 * fluidr3_gm's 141 MB fits. So nothing measured forces an mmap, and
 * building one on a guessed constraint is how this project has gone
 * wrong before.
 *
 * DEFERRED, NOT DISMISSED: if a P3 run shows startup time or
 * footprint is a problem, mmap(MAP_PRIVATE) at sample_pos is close to
 * a drop-in - render_voice.data is already a const short * into a
 * flat pool, so nothing in the renderer would change.
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

/* --- the output device ---------------------------------------------- */

/*
 * OPEN /dev/dsp - WITH O_EXCL FOR THE RESERVED MIDI SLOT.
 *
 * vsound keeps one slot past the ordinary VSOUND_MAX_CHAN for a MIDI
 * synth, and the way to ask for it is O_EXCL on the ordinary node:
 * the VFS gives that flag no meaning on a device and strips it only
 * after the driver's open() has seen it, no OSS client passes it, and
 * it costs no second minor. vsound_chan.h's VSOUND_MIDI_SLOT comment
 * has the whole argument; this is the client side of it.
 *
 * THE ANSWERS, with `excl':
 *   0        the slot, and nothing else can be given it
 *   -ENODEV  vsound was loaded without vsound_midi=1 - no such slot.
 *            Returned to the caller SILENTLY so acquire_dsp() can
 *            say the right thing once and fall back.
 *   -EBUSY   another process holds it, which means another vmidid.
 *
 * THE NEGOTIATION ORDER IS FORMAT, CHANNELS, SPEED - the same as
 * vdiscd_audio.c's open_channel(), and the same as every OSS client
 * in the reference trees. Setting speed before format can leave a
 * driver resampling to a rate it then changes.
 */
static int
open_dsp(const char *dev, int rate, int verbose, int excl)
{
    int fd, v;

    /*
     * O_NONBLOCK ON THE OPEN, CLEARED THE INSTANT IT SUCCEEDS -
     * 2026-09-28, matching `vsoundd' and `vdiscd_audio.c'.
     *
     * WITHOUT IT A PCI CARD'S OPEN BLOCKS. `esssolo1' loops on
     * schedule() while the device is taken and returns EBUSY only
     * when this flag is set; es1371, es1370, cmpci, sonicvibes and
     * maestro are the same shape. CLAUDE.md section 5 has the
     * survey and the days it cost before it was understood.
     *
     * IT IS WHY THIS DAEMON'S FAILURE LOOKED LIKE A DAEMON FAULT.
     * On 2026-09-28 a plan handed vmidid the CARD by mistake (the
     * `out_token()' scope bug, fixed separately) while `vsoundd'
     * held it; with no flag the open did not refuse, and the first
     * write returned EIO with no explanation. With the flag it
     * would have said "all vsound channels are in use" and the
     * afternoon would have been a minute.
     *
     * AND CLEARING IT IS NOT OPTIONAL. `audio.c:225' tests
     * `file->f_flags & O_NONBLOCK' on EVERY WRITE, so leaving it
     * set turns a blocking write into EAGAIN - and the blocking
     * write is what paces the render loop against the card.
     */
    fd = open(dev, O_WRONLY | O_NONBLOCK | (excl ? O_EXCL : 0));
    if (fd < 0) {
        int e = errno;

        if (excl && e == ENODEV) {
            errno = e;
            return -1;
        }
        fprintf(stderr, "vmidid: %s: %s\n", dev, strerror(e));
        /*
         * THREE ERRNOS MEAN BUSY, NOT ONE. EBUSY for esssolo1,
         * es1371, es1370, cmpci, sonicvibes and the legacy path;
         * EWOULDBLOCK for maestro; EAGAIN for emu10k1 v0.20a,
         * which is the P3's DEFAULT driver. Testing EBUSY alone
         * left two real machines here printing a bare strerror.
         */
        if (e == EBUSY || e == EAGAIN || e == EWOULDBLOCK)
            fprintf(stderr, excl
                ? "vmidid: the reserved MIDI slot is already held -"
                  " is another vmidid running?\n"
                : "vmidid: all vsound channels are in use\n");
        errno = e;
        return -1;
    }

    /* AND OFF AGAIN: from here the write blocking IS the pacing.
     * See the comment on the open. */
    fcntl(fd, F_SETFL, 0);

    v = AFMT_S16_LE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &v) < 0 || v != AFMT_S16_LE) {
        fprintf(stderr, "vmidid: format is 0x%x, wanted S16_LE\n", v);
        close(fd);
        return -1;
    }

    v = SYNTH_CHANNELS;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) < 0 || v != SYNTH_CHANNELS) {
        fprintf(stderr, "vmidid: %d channels, wanted %d\n", v, SYNTH_CHANNELS);
        close(fd);
        return -1;
    }

    v = rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &v) < 0) {
        fprintf(stderr, "vmidid: SPEED failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    /*
     * A RATE WE DID NOT ASK FOR IS NOT FATAL, BUT THE RENDERER MUST
     * BE TOLD. render_init takes the rate and every pitch in the synth
     * is computed from it; rendering at 44100 and playing at 22050
     * transposes the whole performance down an octave. So the caller
     * uses what came back, and says so.
     */
    if (v != rate && verbose)
        fprintf(stderr, "vmidid: card gave %d Hz, asked %d - rendering at %d\n",
                v, rate, v);
    return fd;
}

/*
 * THE RESERVED SLOT IF THERE IS ONE, AN ORDINARY CHANNEL IF NOT.
 *
 * `*reserved' is the policy and the record: 1 on the way in means
 * "ask for the MIDI slot", and it is cleared the one time vsound
 * answers -ENODEV, so every later reopen goes straight to an ordinary
 * channel and the kernel is not asked (and does not log) again.
 *
 * THE FALL-BACK IS SAID OUT LOUD, UNCONDITIONALLY, because it changes
 * what a run means: a synth on an ordinary channel is one of the four
 * a game may need, so lxdoom with music is three clients and a fourth
 * could be refused. The line goes to stderr, which load.sh sends to
 * DAEMON.LOG, and vsound logs the same -ENODEV to the kernel side -
 * so the capture records it whichever half is read. A user has no
 * such line in front of them; design/09 records that the cleanup
 * step still needs a way to show it in the tools.
 */
/*
 * WHAT `-o' LAST RESOLVED TO, for the messages.
 *
 * Every line that names the output device must name the DEVICE, not
 * the token: "vmidid: @VSOUND@: No such file" would be nonsense to a
 * user and a wrong answer to the question the line is asked for.
 * acquire_dsp() fills this on every resolve, so it is current even
 * after a rebind.
 */
static char dsp_resolved[256] = "";
/* 1 when the last acquire_dsp() was told to WAIT - vsound not usable
 * yet, or its pump waiting for the very card we hold (design/54 6b) -
 * so the "no channel" line names what is awaited, not a busy card. */
static int resolve_waited;

/*
 * WHAT WE HAVE ALREADY SAID ABOUT THE CURRENT DEVICE.
 *
 * These replace two `*reserved = 0' latches that made a warning
 * permanent - see acquire_dsp(). A warning must not repeat after
 * every 3000 ms release, and it must not be silenced FOREVER by one
 * open: both are cleared when the condition they describe stops
 * being true, so a rebind reports the new device honestly.
 */
static int said_not_vsound;     /* "this is a real card" */
static int said_no_slot;        /* "vsound has no reserved MIDI slot" */


/*
 * WHAT WE ACTUALLY GOT, as opposed to what we asked for.
 *
 * `reserved' is the REQUEST and it no longer gets cleared, so it
 * cannot answer "which channel am I on" any more - it is 1 for the
 * whole run. This is set by acquire_dsp() from the result: 1 only
 * when vsound answered VSOUND_IOC_STAT on an O_EXCL open, which is
 * the one case where we truly hold the MIDI slot.
 *
 * THE THREE REPORTING LINES READ THIS. They used to read `reserved'
 * and were right only because the latch made request and outcome
 * the same thing; with a device that can change, they are not.
 */
static int have_slot;

static int
acquire_dsp(const char *spec, int rate, int verbose, int *reserved)
{
    int  fd;
    char devbuf[256];
    const char *dev;

    /*
     * RESOLVE `-o' HERE, ON EVERY ACQUIRE - design/43 part A.
     *
     * `spec' may be the literal "@VSOUND@", in which case the answer
     * depends on whether vsound is loaded RIGHT NOW. Both callers
     * come through here - the first open and every reacquire after a
     * release - so a vsound that arrives or departs mid-session is
     * picked up without a restart.
     *
     * 0 MEANS WAIT, AND WE MUST NOT SUBSTITUTE A CARD. The plan
     * emits the token only when it is also loading vsound, so this
     * is a load in progress; the caller's retry loop asks again in
     * 250 ms. Playing to a real card instead would be a different
     * mode with a release cycle that is wrong for it - design/36
     * row 53, where it cost "the odd note here or there of music".
     */
    switch (vlhe_resolve_out(spec, devbuf, sizeof devbuf)) {
    case 1:
        dev = devbuf;
        strcpy(dsp_resolved, devbuf);   /* same size, checked by resolve */
        resolve_waited = 0;
        break;
    case 0:
        resolve_waited = 1;
        errno = ENODEV;
        return -1;
    default:
        fprintf(stderr, "vmidid: cannot resolve output device `%s'\n", spec);
        errno = EINVAL;
        return -1;
    }

    if (*reserved) {
        fd = open_dsp(dev, rate, verbose, 1);
        if (fd >= 0) {
            /*
             * IS THIS vsound AT ALL? - 2026-09-24, design/36 row 53.
             *
             * O_EXCL is vsound's request for the reserved MIDI slot,
             * and vsound answers it. A REAL card's driver sees the
             * same flag, ignores it, and opens - so with vsound absent
             * this open SUCCEEDED and the daemon believed it held the
             * reserved slot on the raw SB16. It then released the card
             * after two seconds of silence, as the slot design wants,
             * and lost it to sndserv every time: the user heard "the
             * odd note here or there of music". No line said any of
             * this, because every message on this path assumes vsound
             * is loaded.
             *
             * VSOUND_IOC_STAT is a read-only ioctl only vsound answers.
             * A card driver returns an error for it, and that is the
             * whole test. Not a bug in the release cycle - the user's
             * call - but the daemon must know which device it is on,
             * and say so.
             */
            struct vsound_stat st;

            if (ioctl(fd, VSOUND_IOC_STAT, &st) != 0) {
                /*
                 * SAID ONCE PER DEVICE, NOT ONCE PER RUN, AND THE
                 * LATCH IS GONE - design/43 part A, step 3.
                 *
                 * This used to do `*reserved = 0' with the comment
                 * "never claim the slot again", which was right when
                 * `-o' was a fixed path: one device for the whole
                 * run, so one answer. It is WRONG now that
                 * `@VSOUND@' re-resolves - a synth that started on a
                 * card while vsound was still loading would clear
                 * the flag, then reach vsound on the next release
                 * and take an ORDINARY channel forever, eating one
                 * of the four a game may need. Mixing correctly, in
                 * the wrong slot, with the only evidence a warning
                 * printed minutes earlier.
                 *
                 * NOTHING IS LOST BY ASKING AGAIN. The ioctl runs on
                 * every acquire regardless, and O_EXCL on a card is
                 * ignored rather than refused - so the retry costs
                 * one flag in an open we were making anyway.
                 */
                if (!said_not_vsound) {
                    fprintf(stderr,
                        "vmidid: %s is NOT vsound - a real sound card."
                        " Playing straight to it: nothing else can use it"
                        " while a note sounds, and every rest hands it to"
                        " whoever asks next. Load vsound for mixing.\n",
                        dev);
                    said_not_vsound = 1;
                }
                have_slot = 0;          /* a card ignored the flag */
            } else {
                /* ON VSOUND, HOLDING THE SLOT. Clear both, so
                 * leaving for a card - or meeting a vsound without
                 * the parameter - is reported afresh. */
                said_not_vsound = 0;
                said_no_slot    = 0;
                have_slot       = 1;    /* vsound answered: this IS the slot */
            }
            return fd;
        }
        if (errno != ENODEV)
            return -1;
        /*
         * -ENODEV IS vsound SAYING IT HAS NO SLOT, and that is a
         * fact about how the module was LOADED (`vsound_midi=1' or
         * not), not about which device we reached. So this one is
         * still remembered - but per acquire rather than forever,
         * because a vsound unloaded and reloaded WITH the parameter
         * should be asked again.
         *
         * `*reserved' is therefore no longer cleared here either:
         * the fall-through below takes an ordinary channel for THIS
         * open, and the next acquire asks for the slot afresh. The
         * message is rate-limited instead, or a machine without the
         * parameter would print it after every release.
         */
        if (!said_no_slot) {
            fprintf(stderr,
                "vmidid: %s has no reserved MIDI slot (vsound loaded without"
                " vsound_midi=1 - load.sh -M?)\n"
                "vmidid: taking an ordinary channel instead - one of the"
                " channels a game could need\n", dev);
            said_no_slot = 1;
        }
    }
    have_slot = 0;              /* an ordinary channel, by either route */
    return open_dsp(dev, rate, verbose, 0);
}

/* --- the pump -------------------------------------------------------- */

/*
 * A MISSING SOUNDFONT IS NOT A BUG IN THIS PROGRAM, AND MUST NOT READ
 * LIKE ONE.
 *
 * THE FONT IS NOT IN THE BUNDLE, deliberately (the user's decision,
 * 2026-09-07). It is 9.8 MB of third-party, gitignored material that
 * does not change between runs, where everything mkbundle.sh stages
 * is built from tracked source - so re-transferring it to test a
 * 280 KB daemon is the wrong shape, and worse on the Acer's five-hop
 * chain than on the P3's CF card. It is installed on the target ONCE,
 * out of band, exactly as ide-dma, the wacom bridge and pv already
 * are.
 *
 * The consequence is that "no soundfont" is the FIRST thing a new
 * machine will hit, and a bare strerror() makes an ordinary
 * installation step look like a broken binary. So every path that
 * fails for want of a font says where it is expected and that it is
 * installed separately.
 *
 * SF2 IS NOT SHIPPED AT ALL, and that is a licensing decision as well
 * as a size one - design/p3-midi-plan.md section 7. "Bring your own,
 * here is one tested to work" is how timidity, DOSBox and fluidsynth
 * all behave.
 */
/*
 * -s FILE[@BANK] for the fonts above the first: split the bank offset
 * off. smf2wav has the same helper and the same reason to keep it
 * local (see load_pool above). Returns -1 on a bad bank.
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

/*
 * BUT A FONT THAT IS THERE AND CANNOT BE READ IS NOT MISSING - 2026-10-06,
 * Red Hat 6.0: an installed vmidid runs as the vlhe account, the font
 * was in /home/nova, Red Hat makes a home drwx------, and this said
 * "NOT part of this bundle ... Expected: /usr/local/share/sounds/sf2" -
 * sending the user to look for a file they had. So: there but not
 * readable, or a directory on the way that will not let us through,
 * is said as that.
 */
static void
no_font(const char *tried)
{
    struct stat sb;

    if (tried != NULL)
        fprintf(stderr, "vmidid: cannot use the soundfont at %s\n", tried);
    if (tried != NULL
        && (stat(tried, &sb) == 0 ? access(tried, R_OK) != 0
                                  : errno == EACCES)) {
        fprintf(stderr,
            "vmidid: this account (uid %ld) cannot read it, or cannot get\n"
            "        into a directory on the way to it.\n"
            "        The daemons run as their own account: keep fonts where\n"
            "        everyone can read them - e.g. /usr/local/share/sf2 - with\n"
            "        every directory on the way open to others (a home\n"
            "        directory often is not).\n", (long) getuid());
        return;
    }
    fprintf(stderr,
        "vmidid: a SoundFont is REQUIRED and is NOT part of this bundle.\n"
        "        It is installed on the machine separately, once - README\n"
        "        says how. This is an installation step, not a fault in\n"
        "        vmidid.\n"
        "        Expected: %s\n", SF2_INSTALL_PATH);
}

static void
usage(const char *me)
{
    fprintf(stderr,
        "usage: %s -s FONT.sf2 [-d DEV] [-o DSP] [-b FRAMES] [-q BLOCKS]\n"
        "          [-i MS] [-g GAIN] [-R MS] [-p VOICES] [-A on|off] [-a SPEC]\n"
        "          [-r RATE] [-F MODE] [-L LAW] [-E on|off|reverb|chorus]\n"
        "          [-B on|off] [-M fast|reference] [-v] [-V]\n"
        "  -s FONT  the SoundFont to play (required). Repeat to stack\n"
        "           more above it: -s song.sf2@1 puts that font's bank 0\n"
        "           in MIDI bank 1; later fonts are searched first.\n"
        "  -d DEV   MIDI input   (default %s)\n"
        "  -o DSP   audio output (default %s)\n"
        "  -b N     frames per block  (default %d)\n"
        "  -q N     blocks rendered ahead, 0 = none (default %d).\n"
        "           N > 0 is the old queue, kept for comparison only:\n"
        "           it adds N-1 blocks of delay and protects nothing\n"
        "  -i MS    idle ms before an all-notes-off (default %d, 0 = off)\n"
        "  -g GAIN  master gain, 1.0 = unity, 0.5 = default, 2.0 = max;\n"
        "           the output clips and counts whatever does not fit\n"
        "  -R MS    silent ms before the channel is released so a pump\n"
        "           running -R can drop the card (default %d, 0 = hold)\n"
        "  -p N     max voices, 1..%d (default %d). SIGUSR1 lowers it\n"
        "           by 8 while running and SIGUSR2 raises it, so a\n"
        "           machine that cannot keep up does not need a\n"
        "           restart - which would drop the channel\n"
        "  -A on|off automatic voice reduction (default on), as\n"
        "           TiMidity: when this synth's own output buffer drains,\n"
        "           shed release tails and lower the ceiling; put it back\n"
        "           when healthy. A SIGUSR nudge ends it\n"
        "  -a SPEC  its settings, name=value[,...]: drain (50%%),\n"
        "           emergency (10%%) and healthy (75%%) buffer fills,\n"
        "           settle (100 ms), floor (8), tails (1 = shed release\n"
        "           tails first); emergency < drain < healthy\n"
        "  -r RATE  sample rate (default %d)\n"
        "  -F MODE  velocity-filter default: awe (default), 2.01, 2.04,\n"
        "           none - or 1, 2, 3, 0. Which spec a font is played\n"
        "           against; none is what fluidsynth does\n"
        "  -L LAW   VOLUME CURVE for velocity, CC 7 and CC 11 - NOT the\n"
        "           same setting as -F above:\n"
        "             spec   (default) the SoundFont standard,\n"
        "                    gain (v/127)^2 - what fluidsynth plays\n"
        "             linear gentler, Sound Blaster era: gain v/127,\n"
        "                    what the OPL3 and the original AWE32\n"
        "                    drivers played. Soft notes lose half as\n"
        "                    many decibels - 6 dB louder at velocity\n"
        "                    64 - which suits DOS-era games, balanced\n"
        "                    against those cards\n"
        "           or 0, 1\n"
        "  -E on|off|reverb|chorus  reverb and chorus (default on);\n"
        "           off is the escape on a machine without the headroom,\n"
        "           chorus alone keeps some width for most of the saving\n"
        "           (the reverb is nearly all the cost)\n"
        "  -B on|off filter bypass: skip the low-pass on a voice whose\n"
        "           cutoff is above anything its sample can carry\n"
        "           (default off)\n"
        "  -M fast|reference  modulation envelope: fast (default) skips it\n"
        "           while it modulates nothing and catches it up exactly\n"
        "           when it starts to; reference steps it every frame, as the\n"
        "           spec does. Identical on everything tested\n"
        "  -v       report what was negotiated and a line per second\n"
        "  -V       per-voice trace at note-on (very loud)\n",
        me, MIDI_DEV_DEFAULT, DSP_DEV_DEFAULT,
        BLOCK_FRAMES_DEFAULT, QUEUE_BLOCKS_DEFAULT,
        IDLE_PANIC_MS_DEFAULT, RELEASE_MS_DEFAULT,
        VMIDID_MAX_VOICES, RENDER_DEFAULT_VOICES, SYNTH_RATE);
}

static void
trace_voice(const render_voice *v, void *arg)
{
    long rate = *(long *) arg;

    fprintf(stderr,
        "  ch%-2d key %3d vel %3d  D%5ld A%5ld H%5ld Dec%5ld R%5ld  %s\n",
        v->channel, v->key, v->velocity,
        v->delay_frames   * 1000L / rate,
        v->attack_frames  * 1000L / rate,
        v->hold_frames    * 1000L / rate,
        v->decay_frames   * 1000L / rate,
        v->release_frames * 1000L / rate,
        v->looping ? "looping" : "one-shot");
}

/*
 * FRAMES TO MILLISECONDS WITHOUT OVERFLOWING. frames * 1000 leaves an
 * unsigned 32-bit long at 4.29M frames - 97 seconds of audio - and
 * the exit line printed 34893 blocks as "7770 ms" on a 202-second run
 * (tests/logs/2026-09-07-vmidi-86box-playmidi-nox-p32-vs-p64). Divide
 * first and carry the remainder, as render.c does for every product
 * of this shape; the result is exact.
 */
static unsigned long
frames_to_ms(unsigned long frames, int rate)
{
    unsigned long r = (unsigned long) rate;
    return (frames / r) * 1000UL + ((frames % r) * 1000UL) / r;
}

static long
ms_since(const struct timeval *t0)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (now.tv_sec - t0->tv_sec) * 1000L
         + (now.tv_usec - t0->tv_usec) / 1000L;
}

/*
 * THE PID FILE - vmidid.pid in the run directory's ctl/ (design/54
 * D44; vlhe_status_ctldir()), which is where lib/vlhe_status.c looks.
 *
 * WRITTEN LOCALLY RATHER THAN SHARED, deliberately. vmidi/ links
 * nothing from vsound/ today and eight lines is not a reason to
 * start: the coupling would outlast the convenience. THE PATH IS THE
 * CONTRACT, and it is duplicated here on purpose - if it ever moves,
 * it moves in vlhe_status.c and here.
 *
 * NOT FATAL ON FAILURE. A daemon that cannot write /var/run still
 * synthesises; it is only harder to see from the GUI.
 */
/*
 * THE PID FILE IS vsound/vlhe_status.c's, SHARED RATHER THAN COPIED.
 *
 * IT USED TO BE A PRIVATE COPY HERE, and the copy drifted. That file
 * grew a "refuse to start when one is already running" check on
 * 2026-09-23; this tree never got it, so pressing Load twice left
 * vsoundd and vdiscd correctly refusing while vmidid clobbered its
 * own pidfile, failed to open the MIDI device the first one held, and
 * exited - leaving the file naming a corpse and "stale pid file - it
 * died" on the Status page.
 *
 * THE USER'S PRECEDENT, and it is the reason this is shared rather
 * than fixed twice: "Duplicated code drifting is why 3 scripts turned
 * into 1" - the per-machine load.sh wrappers, factored SO THEY COULD
 * NOT DRIFT and which drifted anyway (CLAUDE.md section 1).
 *
 * A FIRST ANSWER HERE CLAIMED IT COULD NOT BE SHARED, from the
 * directory layout alone. The user asked why, and the answer was that
 * nothing stopped it: vlhe_status.c includes only libc and its own
 * header. The one real friction was -ansi hiding POSIX's kill(),
 * which -D_POSIX_SOURCE answers; the Makefile records both.
 */

/*
 * A STARTUP REFUSAL IS NOT A DEATH - 2026-10-01. The pid file is
 * written early (below) so a second instance can be refused, and the
 * clean exit removes it so that a file left behind means the daemon
 * DIED - the Status page's "stale pid file - it died". But the twenty
 * refusals between the write and the main loop (a bad flag, no font,
 * no output device) exited through a bare `return 1' and left the
 * file naming a corpse; on 86Box a Restart with vsound unloaded did
 * exactly that, and the row said "it died" about a daemon that had
 * declined to start. Every one of them comes through here.
 */
static int
startup_failed(void)
{
    vlhe_status_remove_pidfile("vmidid");
    return 1;
}

int
main(int argc, char **argv)
{
    const char *sfname = NULL;
    /* Fonts stacked above the first: -s repeated, FILE[@BANK] -
     * render.h on the shape, design/22 step 1. */
    const char *xname[RENDER_MAX_FONTS];
    sf2_file    xsf[RENDER_MAX_FONTS];
    int         nx = 0;
    const char *midi_dev = MIDI_DEV_DEFAULT;
    const char *dsp_dev  = DSP_DEV_DEFAULT;
    int   block_frames = BLOCK_FRAMES_DEFAULT;
    int   queue_blocks = QUEUE_BLOCKS_DEFAULT;
    int   idle_ms      = IDLE_PANIC_MS_DEFAULT;
    int   release_ms   = RELEASE_MS_DEFAULT;
    /* Ask for the reserved slot; cleared by acquire_dsp() if there is
     * none, after which every reopen is an ordinary one. */
    int   reserved     = 1;
    /* Which velocity-filter default to play - sf2mod.h, design/18
     * section 10f. AWE unless -F says otherwise. */
    unsigned velfilter = SF2_MOD_VF_AWE;
    unsigned law       = SF2_MOD_LAW_SPEC;
    int   effects      = RENDER_FX_BOTH;  /* -E: a RENDER_FX_* mask, design/21 */
    int   bypass       = 0;       /* -B: the filter bypass, design/20 3b */
    int   menv         = RENDER_MENV_FAST;  /* -M; fast by default since the corpus proof, design/54 D28 */
    int   auto_voices  = 1;       /* -A: automatic voice reduction, design/21 16 */
    const char *auto_spec = NULL; /* -a: its settings, applied after init */
    long  master       = 0;       /* -g, in 1/1024ths; 0 = the default */
    /*
     * 0 MEANS "LEAVE IT ALONE", not "no voices". The renderer's own
     * default is RENDER_DEFAULT_VOICES and render_set_max_voices is only
     * called when -p was given, so an unused knob cannot change what
     * the synth does.
     */
    int   max_voices   = 0;
    /*
     * TWO RATES, AND COLLAPSING THEM WAS A BUG - design/43 section 5d.
     *
     * `want_rate' is what the USER asked for and never changes;
     * `rate' is what the current device actually granted. A single
     * variable cannot tell "22050 because the user chose it" from
     * "22050 because an SB Pro clamped stereo", and the first must
     * survive the card going away while the second must not. Every
     * open asks for `want_rate' and adopts what comes back, so a
     * card's clamp is undone the moment the daemon reaches vsound.
     */
    int   want_rate    = SYNTH_RATE;
    int   rate         = SYNTH_RATE;
    int   verbose = 0, vtrace = 0;
    int   i;
    FILE         *sfp;
    sf2_file      sf;
    short        *pool = NULL;
    long          pool_frames = 0;
    long          trace_rate;
    render_state  r;
    midi_parser   parser;

    int   midi_fd = -1, dsp_fd = -1, ctl_fd = -1;
    short *queue = NULL;
    int    queued = 0;            /* blocks rendered, not yet written */
    unsigned char inbuf[256];
    struct sigaction sa;
    struct timeval start, last_event, last_report, last_sound, last_retry;
    /* THE OUTPUT FOLLOW - design/54 6b step 2. When it last re-resolved
     * the output, and whether it closed the device to move, so the
     * reopen happens without waiting for the next MIDI byte. */
    struct timeval last_follow;
    int   follow_reopen = 0;
    unsigned long blocks_out = 0, bytes_in = 0, panics = 0, short_writes = 0;
    unsigned long releases = 0, dropped = 0;
    int   said_no_channel = 0;

    /*
     * THE PREVIOUS REPORT'S TOTALS, so each line can print a DELTA.
     *
     * Cumulative counters cannot show a stall. The first run on any
     * machine (tests/logs/2026-09-07-vmidi-86box-first-run-starved)
     * starved for nine seconds out of thirty-five and the console
     * showed it only as numbers that grew more slowly - which had to
     * be recovered afterwards by differencing block counts off a
     * SCREENSHOT. A delta beside the total makes it readable while it
     * is happening.
     *
     * `was_silent' USED TO BE HERE AND WAS DEAD - assigned, never
     * read, left over from a silence-transition log that was never
     * written. Same class as usb-midi's cin_f0ff table: a value that
     * is only correct because nothing consumes it. Removed rather
     * than wired up, because the report below prints the voice count
     * every second anyway and a transition line would say less.
     */
    unsigned long prev_blocks = 0, prev_bytes = 0, prev_msgs = 0;

    /*
     * LINE BUFFERING, AND IT IS NOT COSMETIC.
     *
     * load.sh redirects this daemon's stdout to DAEMON.LOG, and libc
     * BLOCK-buffers a FILE - so output sits in a 4 KB buffer until
     * the process exits. Found on 86Box 2026-09-19: DAEMON.LOG held
     * vsoundd and vmidid output and NOTHING from vdiscd, which
     * prints a handful of lines and never fills a block. Its attach
     * banner and the control channel's own logging were invisible
     * while it ran.
     *
     * THIS DAEMON ONLY LOOKED FINE BY ACCIDENT - it reports often
     * enough to cross 4 KB early. It never flushed either.
     *
     * A LOG YOU CANNOT READ UNTIL THE PROGRAM EXITS IS NOT A LOG for
     * a daemon, and it is worst exactly when it is most needed:
     * diagnosing a hang, where the evidence is in the address space
     * of the process that will not exit.
     */
    setvbuf(stdout, (char *) 0, _IOLBF, 0);

    /* THE PID FILE, so the Status page can tell running from
     * stopped. Until 2026-09-19 no daemon wrote one and the page
     * said "no pid file" for all three while all three ran.
     *
     * AND IT CAN REFUSE. -2 means another vmidid is already running;
     * starting a second would take the device the first holds, fail,
     * and leave the pidfile naming a corpse. */
    if (vlhe_status_write_pidfile("vmidid") == -2) {
        fprintf(stderr, "vmidid: already running - not starting a"
                        " second one\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            /* The first -s is the base font at bank 0; each later one
             * stacks above it with its own offset. */
            if (sfname == NULL)
                sfname = argv[++i];
            else if (nx < RENDER_MAX_FONTS - 1)
                xname[nx++] = argv[++i];
            else {
                fprintf(stderr, "vmidid: at most %d fonts\n", RENDER_MAX_FONTS);
                return startup_failed();
            }
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            midi_dev = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            dsp_dev = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            block_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            queue_blocks = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            idle_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            double g = atof(argv[++i]);
            if (g < 0.001 || g > 2.0) {
                fprintf(stderr, "vmidid: -g must be 0.001..2.0 (1.0 = unity, 0.5 = the default)\n");
                return startup_failed();
            }
            master = (long) (g * 1024.0 + 0.5);
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            release_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            int f = sf2_mod_vf_parse(argv[++i]);
            if (f < 0) {
                fprintf(stderr, "vmidid: -F %s: awe, 2.01, 2.04 or none\n",
                        argv[i]);
                return startup_failed();
            }
            velfilter = (unsigned) f;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            int f = sf2_mod_law_parse(argv[++i]);
            if (f < 0) {
                fprintf(stderr, "vmidid: -L %s: spec or linear\n", argv[i]);
                return startup_failed();
            }
            law = (unsigned) f;
        } else if (strcmp(argv[i], "-E") == 0 && i + 1 < argc) {
            i++;
            effects = render_fx_parse(argv[i]);
            if (effects < 0) {
                fprintf(stderr, "vmidid: -E %s: on, off, reverb or"
                                " chorus\n", argv[i]);
                return startup_failed();
            }
        } else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "fast") == 0)           menv = RENDER_MENV_FAST;
            else if (strcmp(argv[i], "reference") == 0) menv = RENDER_MENV_REFERENCE;
            else {
                fprintf(stderr, "vmidid: -M %s: fast or reference\n", argv[i]);
                return startup_failed();
            }
        } else if (strcmp(argv[i], "-B") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "on") == 0)       bypass = 1;
            else if (strcmp(argv[i], "off") == 0) bypass = 0;
            else {
                fprintf(stderr, "vmidid: -B %s: on or off\n", argv[i]);
                return startup_failed();
            }
        } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "on") == 0)       auto_voices = 1;
            else if (strcmp(argv[i], "off") == 0) auto_voices = 0;
            else {
                fprintf(stderr, "vmidid: -A %s: on or off\n", argv[i]);
                return startup_failed();
            }
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            auto_spec = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            max_voices = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            /* BOTH: the ask, and the starting assumption until a
             * device says otherwise. */
            want_rate = rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-V") == 0) {
            vtrace = 1;
        } else {
            usage(argv[0]);
            return startup_failed();
        }
    }

    /*
     * THE BUILD STAMP AND THE COMMAND LINE, BEFORE ANYTHING CAN FAIL.
     *
     * ADDED 2026-09-07 AFTER A RUN COULD NOT BE EXPLAINED. Two boots
     * were made with -p 32 and -p 16; both reported "64 voices
     * (full)" and both actually reached 62-63, so the cap did not
     * apply. Nothing in any captured file could say WHY: the staged
     * daemon was verified to support -p, the staged load.sh was
     * verified to pass $VMIDID_ARGS, and the one line that would have
     * settled it - load.sh's own "vmidid running" message - goes to
     * the console and is captured nowhere.
     *
     * THE MODULES HAVE HAD THIS SINCE 2026-08-27, for the same
     * reason. vsound_dev.c:2319 records what its absence cost: twice
     * in one day a run tested a STALE module and the traces were read
     * as findings before anyone noticed. vsoundd prints one too
     * (vsoundd.c:274). The daemon had the same exposure and none of
     * the protection.
     *
     * __DATE__ and __TIME__ are set by the compiler at every
     * compilation, so this cannot be stale and needs nobody to
     * remember it. Compare it against what `make' printed; if they
     * differ, the running binary is not the one just built.
     *
     * THE ARGV ANSWERS THE OTHER HALF. A build stamp says WHICH
     * binary; the argv says what it was ASKED to do. The later banner
     * reports what the renderer TOOK - "64 voices (full)" - so a run
     * where the two disagree is a run where an argument did not
     * arrive, which is exactly the case that could not be diagnosed.
     *
     * NOT GATED ON -v, AND BEFORE THE FIRST FAILURE. A daemon that
     * dies at startup - no soundfont, a busy /dev/dsp, a /dev/vmidi
     * that does not exist - is the case that most needs both lines,
     * and gating them on a flag or printing them after the device
     * opens would withhold them exactly then. Two lines per run.
     */
    {
        int a;

        fprintf(stderr, "vmidid: built %s %s\n", __DATE__, __TIME__);
        /* AN ARGUMENT WITH A SPACE IS QUOTED - 2026-10-01. A font path
         * such as "Creative (emu10k1)8MBGMSFX.SF2" printed bare reads
         * as two arguments, and this line is the record of whether it
         * ARRIVED as one (vlhe_apply.c's split, design/47 M5). */
        fprintf(stderr, "vmidid: argv:");
        for (a = 0; a < argc; a++)
            fprintf(stderr, strchr(argv[a], ' ') != NULL ? " \"%s\"" : " %s",
                    argv[a]);
        fprintf(stderr, "\n");
    }

    if (sfname == NULL) {
        usage(argv[0]);
        no_font(NULL);
        return startup_failed();
    }
    /*
     * BOUND THE KNOBS. They exist to be measured with, which means
     * they will be given wrong values on purpose as well as by
     * accident - and a zero block size is an infinite loop, not an
     * error message.
     */
    if (block_frames < 16 || block_frames > BLOCK_FRAMES_MAX) {
        fprintf(stderr, "vmidid: -b must be 16..%d\n", BLOCK_FRAMES_MAX);
        return startup_failed();
    }
    if (queue_blocks < 0 || queue_blocks > QUEUE_BLOCKS_MAX) {
        fprintf(stderr, "vmidid: -q must be 0..%d (0 = no render-ahead)\n",
                QUEUE_BLOCKS_MAX);
        return startup_failed();
    }
    /* The ceiling is 46340, not 48000: voice_set_pitch's last product
     * is bounded by rate*rate (design/24 B2, render.h). */
    if (rate < RENDER_RATE_MIN || rate > RENDER_RATE_MAX) {
        fprintf(stderr, "vmidid: -r must be %d..%d\n",
                RENDER_RATE_MIN, RENDER_RATE_MAX);
        return startup_failed();
    }

    sfp = fopen(sfname, "rb");
    if (sfp == NULL) {
        fprintf(stderr, "vmidid: %s: %s\n", sfname, strerror(errno));
        no_font(sfname);
        return startup_failed();
    }
    if (sf2_load(&sf, sfp) < 0) {
        fprintf(stderr, "vmidid: %s: not a SoundFont this can read\n", sfname);
        no_font(sfname);
        fclose(sfp);
        return startup_failed();
    }
    pool = load_pool(sfp, &sf, &pool_frames);
    fclose(sfp);
    if (pool == NULL) {
        fprintf(stderr, "vmidid: cannot load the sample pool (%lu bytes)\n",
                (unsigned long) sf.sample_size);
        return startup_failed();
    }

    /*
     * OPEN THE OUTPUT FIRST, and the reason is not arbitrary.
     *
     * /dev/vmidi allows ONE reader (-EBUSY otherwise) and the
     * sequencer starts queueing the moment it is opened. Opening it
     * before we know we can play would take the device, discover the
     * card is busy, and hand back a stream we had already begun to
     * consume - bytes that reached us and went nowhere.
     *
     * The card is also the likelier failure: four channels, and
     * anything else playing can hold them.
     */
    dsp_fd = acquire_dsp(dsp_dev, want_rate, verbose, &reserved);
    if (dsp_fd < 0) {
        free(pool);
        return startup_failed();
    }
    /*
     * Use the rate the card actually gave us - see open_dsp. This is
     * before render_init, so the renderer is built for what we got
     * rather than what we asked; a later device change goes through
     * render_set_rate() instead (design/43 section 5b).
     */
    {
        int v = 0;
        if (ioctl(dsp_fd, SOUND_PCM_READ_RATE, &v) == 0 && v > 0)
            rate = v;
    }
    /*
     * AND A GRANT WE CANNOT RENDER AT IS FATAL HERE, NOT SILENT.
     * RENDER_RATE_MAX is 46340 where a card may legally grant 48000
     * (a no-VRA AC'97 does nothing else - design/42 section 4b), and
     * render_init would take the out-of-range value without
     * complaint. Say which device and which number.
     */
    if (rate < RENDER_RATE_MIN || rate > RENDER_RATE_MAX) {
        fprintf(stderr, "vmidid: %s granted %d Hz and this synth renders"
                        " %d..%d - cannot play on it.\n"
                        "vmidid: load vsound and play through that: it"
                        " accepts any rate the synth can render and"
                        " converts to the card.\n",
                dsp_resolved, rate, RENDER_RATE_MIN, RENDER_RATE_MAX);
        close(dsp_fd);
        free(pool);
        return startup_failed();
    }

    /*
     * THE CONTROL CHANNEL - design/43 section 6b. Opened before the
     * MIDI device so it exists for the whole run, and a FAILURE IS
     * NOT FATAL: it means settings cannot be changed while running,
     * which is exactly where this daemon was before the channel
     * existed. Said once, because a machine with no /var/run would
     * otherwise be silent about why the GUI cannot reach it.
     */
    ctl_fd = vmidid_ctl_open();
    if (ctl_fd < 0 && verbose)
        fprintf(stderr, "vmidid: no control channel (%s) - settings"
                        " cannot be changed while running\n",
                strerror(errno));

    midi_fd = open(midi_dev, O_RDONLY | O_NONBLOCK);
    if (midi_fd < 0) {
        fprintf(stderr, "vmidid: %s: %s\n", midi_dev, strerror(errno));
        if (errno == EBUSY)
            fprintf(stderr, "vmidid: another reader holds it -"
                            " vmidi allows only one\n");
        else if (errno == ENODEV || errno == ENXIO)
            fprintf(stderr, "vmidid: is vmidi loaded, and does %s exist?\n"
                            "        mknod %s c 14 11   (11 unless load.sh set"
                            " VMIDI_MINOR)\n", midi_dev, midi_dev);
        close(dsp_fd);
        free(pool);
        return startup_failed();
    }

    /* One block when the queue is off: the block being rendered is
     * the block being written. */
    queue = (short *) malloc((size_t) block_frames
                             * (queue_blocks > 0 ? queue_blocks : 1)
                             * SYNTH_CHANNELS * sizeof(short));
    if (queue == NULL) {
        fprintf(stderr, "vmidid: cannot allocate the render queue\n");
        close(midi_fd);
        close(dsp_fd);
        if (ctl_fd >= 0)
            vmidid_ctl_close(ctl_fd);
        free(pool);
        return startup_failed();
    }

    render_init(&r, &sf, pool, pool_frames, rate);
    /* THE VOICE ARRAY IS ALLOCATED NOW (design/21 section 17), so
     * render_init() can come back with none. */
    if (r.voices == NULL) {
        fprintf(stderr, "vmidid: no memory for %d voices\n",
                RENDER_DEFAULT_VOICES);
        return startup_failed();
    }
    if (verbose)
        r.log = stderr;     /* what the render did - render.h */
    for (i = 0; i < nx; i++) {
        char  name[512];
        int   bank = 0;
        long  xframes;
        short *xpool;
        FILE *xfp;

        if (font_arg(xname[i], name, sizeof name, &bank) < 0) {
            fprintf(stderr, "vmidid: -s %s: FILE[@BANK], BANK 0..127\n", xname[i]);
            return startup_failed();
        }
        xfp = fopen(name, "rb");
        if (xfp == NULL) {
            fprintf(stderr, "vmidid: %s: %s\n", name, strerror(errno));
            return startup_failed();
        }
        if (sf2_load(&xsf[i], xfp) < 0) {
            fprintf(stderr, "vmidid: %s: not a SoundFont this can read\n", name);
            fclose(xfp);
            return startup_failed();
        }
        xpool = load_pool(xfp, &xsf[i], &xframes);
        fclose(xfp);
        if (xpool == NULL) {
            fprintf(stderr, "vmidid: %s: cannot load the sample pool\n", name);
            return startup_failed();
        }
        render_add_font(&r, &xsf[i], xpool, xframes, bank);
        if (verbose)
            fprintf(stderr, "vmidid: + %s at bank %d, %d presets, %lu MB\n",
                    name, bank, xsf[i].npresets,
                    (unsigned long) (xsf[i].sample_size / (1024UL * 1024UL)));
    }
    render_set_velfilter(&r, velfilter | law);
    render_set_fx(&r, effects);
    render_set_filter_bypass(&r, bypass);
    render_set_menv_mode(&r, menv);
    if (master > 0)
        render_set_master(&r, master);
    /*
     * ONLY WHEN ASKED. render_init has already set the default, so an
     * absent -p leaves the synth exactly as it was. A -p the machine
     * has no memory for stops at what could be allocated, and says so
     * rather than refusing to start.
     */
    if (max_voices > 0) {
        int want = max_voices > VMIDID_MAX_VOICES ? VMIDID_MAX_VOICES
                                                  : max_voices;

        max_voices = vmidid_set_voices(&r, want);
        if (max_voices < want)
            fprintf(stderr, "vmidid: memory for only %d of the %d voices"
                            " asked for\n", max_voices, want);
    }
    /* THE CEILING IT MAY RESTORE TO is whatever -p left - the most
     * the user allowed. design/21 section 16. */
    autovoice_init(&g_av, auto_voices, r.max_voices);
    if (auto_spec != NULL) {
        const char *why = autovoice_parse(&g_av, auto_spec);

        if (why != NULL) {
            fprintf(stderr, "vmidid: -a %s: %s\n", auto_spec, why);
            return startup_failed();
        }
    }
    parser_init(&parser);
    trace_rate = (long) rate;
    if (vtrace) {
        r.trace = trace_voice;
        r.trace_arg = &trace_rate;
    }

    /*
     * sigaction WITH sa_flags = 0, NOT signal().
     *
     * signal() on glibc implies SA_RESTART, which makes a program
     * blocked in read() unkillable by anything but SIGKILL: the
     * kernel restarts the read before the loop can test the flag.
     * vmidicat.c hit exactly this on 2026-09-02 and its comment has
     * the full sequence. The fix is the same here.
     */
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* THE VOICE NUDGES. Same sa_flags = 0, so a signal arriving
     * during the blocking write() interrupts it and the loop's
     * existing EINTR retry picks up - which is how the change takes
     * effect within a block rather than at the next note. */
    sa.sa_handler = on_voice_signal;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    if (verbose) {
        fprintf(stderr, "vmidid: %s -> %s (%s)\n", midi_dev, dsp_resolved,
                have_slot ? "the reserved MIDI slot" : "an ordinary channel");
        if (release_ms > 0)
            fprintf(stderr, "vmidid: releasing the channel after %d ms"
                            " of silence\n", release_ms);
        fprintf(stderr, "vmidid: %s, %d presets, %lu MB of samples\n",
                sfname, sf.npresets,
                (unsigned long) (sf.sample_size / (1024UL * 1024UL)));
        if (queue_blocks > 0)
            fprintf(stderr, "vmidid: %d Hz, %d frames/block (%ld ms),"
                            " %d deep (%ld ms) - THE OLD QUEUE, -q\n",
                    rate, block_frames, block_frames * 1000L / rate,
                    queue_blocks,
                    (long) block_frames * queue_blocks * 1000L / rate);
        else
            fprintf(stderr, "vmidid: %d Hz, %d frames/block (%ld ms),"
                            " no render-ahead\n",
                    rate, block_frames, block_frames * 1000L / rate);
        /*
         * THE VOICE CEILING GOES IN THE BANNER TOO, and it prints the
         * value the renderer TOOK rather than the one asked for -
         * -p 200 is clamped to 64 and a run that does not say so is a
         * run whose configuration has to be guessed afterwards. The
         * -b/-q sweep was readable only because every run stated its
         * own settings; this is the same discipline.
         */
        fprintf(stderr, "vmidid: %d voices%s\n", r.max_voices,
                max_voices > 0 ? " (-p)" : " (the default)");
        /* AND THE AUTOMATIC SETTINGS - they are tunable now, so a
         * capture has to say which ones produced it. */
        if (g_av.on)
            fprintf(stderr, "vmidid: automatic voices on: reduce while the"
                    " buffer is under %d%% and not rising (emergency"
                    " under %d%%), restore at %d%%, settle %d ms, floor %d,"
                    " %s\n", g_av.drain, g_av.emergency, g_av.healthy,
                    g_av.settle_ms, g_av.floor,
                    g_av.tails ? "release tails shed first"
                               : "release tails kept");
        else
            fprintf(stderr, "vmidid: automatic voices off\n");
        /* IN THE BANNER, like the voice ceiling: it changes what a
         * font sounds like, so a capture has to say which was on. */
        /*
         * TWO LINES, AND THAT IS THE POINT. As one line this read
         * "velocity-filter default: awe, law linear" - two unrelated
         * options adjacent, looking like one setting disagreeing with
         * itself, with AWE naming a -F mode AND both -L curves'
         * provenance. The user, 2026-09-18: "awe and linear is
         * confusing".
         *
         * Split, and the law described the way the GUI describes it
         * (vlhe_mod_midi.c calls the control "Volume curve").
         */
        fprintf(stderr, "vmidid: velocity filter: %s (-F)\n",
                sf2_mod_vf_name(velfilter));
        fprintf(stderr, "vmidid: volume curve: %s%s\n",
                sf2_mod_law_desc(law),
                (law & SF2_MOD_LAW_MASK) == SF2_MOD_LAW_LINEAR
                ? " (-L)" : " (default)");
        fprintf(stderr, "vmidid: effects %s, filter bypass %s,"
                        " master gain %.3f%s, modulation envelope %s\n",
                render_fx_name(effects), bypass ? "on" : "off",
                r.master / 1024.0, master > 0 ? " (-g)" : " (default)",
                menv == RENDER_MENV_FAST ? "fast" : "reference (-M)");
    }

    gettimeofday(&start, NULL);
    last_event  = start;
    last_report = start;
    last_sound  = start;
    last_retry  = start;
    last_follow = start;

    /*
     * THE LOOP, AND WHY IT IS SHAPED LIKE THIS.
     *
     * Three things have to happen and only one of them may block:
     *
     *   1. take MIDI bytes whenever there are any        (never blocks)
     *   2. render ahead while the queue has room         (never blocks)
     *   3. write a block to the card                     (BLOCKS - and
     *                                                     that is the
     *                                                     clock)
     *
     * THE WRITE IS THE PACING. /dev/dsp accepts exactly one sample
     * period's worth per period, so a blocking write to it IS the
     * timebase - there is no sleep, no timer, and nothing to drift.
     * That is how vsound's own pump works and why this needs no clock
     * of its own.
     *
     * MIDI IS READ NON-BLOCKING, so a stream that has gone quiet
     * cannot stall the audio. The select() below waits on MIDI only
     * while the queue is FULL - at that point there is nothing to
     * render and the write will block anyway, so sleeping on input
     * costs nothing and keeps us off the CPU.
     */
    while (!stop_now) {
        int n;
        int got_input = 0;

        /* --- 1. drain MIDI ------------------------------------------ */
        for (;;) {
            n = (int) read(midi_fd, inbuf, sizeof inbuf);
            if (n > 0) {
                int k;
                got_input = 1;
                bytes_in += (unsigned long) n;
                for (k = 0; k < n; k++)
                    parser_byte(&parser, inbuf[k], &r);
                gettimeofday(&last_event, NULL);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            /*
             * EAGAIN is the ordinary quiet case. A 0 return cannot
             * happen on the device (a non-blocking read with nothing
             * queued gives -EAGAIN), so it means the input is a
             * regular file at EOF - which is how this is tested
             * against a captured stream.
             */
            break;
        }
        if (n == 0) {
            if (verbose)
                fprintf(stderr, "vmidid: input at EOF\n");
            /*
             * A FILE THAT RAN OUT IS EXACTLY THE MID-PEDAL CASE.
             * Release everything and let the queue drain, rather than
             * cutting the tail off.
             */
            render_panic(&r);
            panics++;
            break;
        }
        if (n < 0 && errno != EAGAIN) {
            fprintf(stderr, "vmidid: read: %s\n", strerror(errno));
            break;
        }

        /* --- no channel: reopen on input, otherwise wait for it ----- */
        /*
         * THE CHANNEL WAS RELEASED (see RELEASE_MS_DEFAULT) and the
         * write below is the clock, so without one there is nothing to
         * pace and nothing to play into. Input is what brings it back.
         *
         * WITH THE RESERVED SLOT THE REOPEN CANNOT FAIL short of the
         * module being unloaded. On an ORDINARY channel it can: the
         * four may all be in use by the time the next song starts.
         * Then the notes are dropped rather than queued - parsing
         * continues so program changes and controllers stay right,
         * but voices are released at once, or they would all sound
         * together when a channel finally appears - and the open is
         * retried every 250 ms while input keeps arriving. Both are
         * said once and counted, not left to be inferred from silence.
         */
        if (dsp_fd < 0) {
            if ((got_input || follow_reopen) &&
                (!said_no_channel || ms_since(&last_retry) >= 250L)) {
                char was[sizeof dsp_resolved];

                strcpy(was, dsp_resolved);
                dsp_fd = acquire_dsp(dsp_dev, want_rate, verbose, &reserved);
                gettimeofday(&last_retry, NULL);
                if (dsp_fd >= 0) {
                    int v = 0;

                    /*
                     * A REBIND IS A FACT ABOUT THE RUN AND MUST BE IN
                     * THE LOG - design/43 part A. `-o @VSOUND@'
                     * resolves afresh every acquire, so the device
                     * can change under a running daemon when vsound
                     * is loaded or unloaded between releases. Without
                     * this line the only evidence would be a rate
                     * that moved, and not even that when the two
                     * devices agree.
                     *
                     * UNCONDITIONAL, not behind -v: it changes what
                     * the run MEANS, like the not-vsound warning
                     * below.
                     */
                    if (was[0] != '\0' && strcmp(was, dsp_resolved) != 0)
                        fprintf(stderr, "vmidid: output moved %s -> %s\n",
                                was, dsp_resolved);

                    /*
                     * THE RENDERER FOLLOWS THE DEVICE - design/43
                     * section 5b, and this used to be a warning that
                     * the pitch would be wrong.
                     *
                     * WHY IT CAN FOLLOW HERE AND NOWHERE ELSE: this
                     * runs after a RELEASE, so render_active() was 0
                     * and nothing is sounding. No voice is holding a
                     * step computed from the old rate, and the reverb
                     * tail that render_set_rate() discards is long
                     * gone - RELEASE_MS_DEFAULT is 3000 ms and its own
                     * comment says that outlasts it.
                     *
                     * WE ASKED FOR want_rate, not `rate', so a card
                     * that clamped us earlier does not pin us: on
                     * vsound this restores the user's choice, and on
                     * an SB Pro it drops us to what it will give.
                     */
                    if (ioctl(dsp_fd, SOUND_PCM_READ_RATE, &v) == 0 &&
                        v > 0 && v != rate) {
                        int got = render_set_rate(&r, v);

                        if (got == v) {
                            if (verbose)
                                fprintf(stderr, "vmidid: reopened at %d Hz"
                                                " (was %d) - renderer"
                                                " followed\n", v, rate);
                            rate = v;
                            trace_rate = (long) rate;
                        } else {
                            /* OUT OF THE RENDERER'S RANGE. Nothing
                             * can be done about it here, so say what
                             * is wrong rather than transposing. */
                            fprintf(stderr, "vmidid: reopened at %d Hz and"
                                            " this synth renders %d..%d -"
                                            " still at %d, pitch will be"
                                            " wrong\n",
                                    v, RENDER_RATE_MIN, RENDER_RATE_MAX,
                                    rate);
                        }
                    }
                    if (verbose)
                        fprintf(stderr, "vmidid: input after %ld ms silent -"
                                        " channel reacquired (%s)\n",
                                ms_since(&last_sound),
                                have_slot ? "reserved MIDI slot"
                                         : "ordinary channel");
                    said_no_channel = 0;
                    follow_reopen   = 0;
                    gettimeofday(&last_sound, NULL);
                } else if (!said_no_channel) {
                    /* NAME WHAT WE ARE WAITING FOR. With `-o
                     * @VSOUND@' and vsound not loaded there is no
                     * resolved name yet, and "cannot reopen" without
                     * saying what would leave a user staring at
                     * silence during a reconfigure with no clue. */
                    if (resolve_waited)
                        fprintf(stderr, "vmidid: waiting for vsound and its"
                                        " pump (vsoundd) - notes are dropped"
                                        " until it is ready\n");
                    else if (dsp_resolved[0] != '\0')
                        fprintf(stderr, "vmidid: cannot reopen %s - notes are"
                                        " dropped until a channel frees\n",
                                dsp_resolved);
                    else
                        fprintf(stderr, "vmidid: waiting for vsound to load -"
                                        " notes are dropped until it does\n");
                    said_no_channel = 1;
                }
            }
            if (dsp_fd < 0) {
                fd_set rfds;
                struct timeval tv;
                int    nfds = midi_fd;

                if (render_active(&r) > 0) {
                    render_panic(&r);
                    dropped++;
                }
                FD_ZERO(&rfds);
                FD_SET(midi_fd, &rfds);
                /*
                 * THE CONTROL CHANNEL IS IN THIS SET, AND LEAVING IT
                 * OUT MADE THE DAEMON DEAF WHEN IDLE - found on
                 * target 2026-09-26 by the user, from the symptom:
                 * *"I stopped playmidi and waited 3 seconds then
                 * applied settings. Thats when it said [vmidid is
                 * not running]. I then started playmidi, applied the
                 * settings. Thats when the settings happened."*
                 *
                 * WHY IT WAS THE WORST POSSIBLE CASE. The timeout
                 * here is NULL unless a channel is being waited for,
                 * so after the 3 s release with no input this slept
                 * INDEFINITELY. The loop never came round, the
                 * drain below it never ran, and a ping went
                 * unanswered until MIDI input happened to arrive -
                 * so the button reported "not running" precisely
                 * when the synth was idle, which is when a user
                 * changes settings.
                 *
                 * AND MY OWN COMMENT ON THAT DRAIN SAID "DRAINED
                 * EVERY PASS", which is true only of passes the
                 * loop makes. The `continue' above it meant there
                 * were none.
                 *
                 * THE TEST COULD NOT HAVE CAUGHT IT: the stand-in
                 * daemon in tests/test_vmidid_ctl.c polls in a
                 * tight loop, so it was shaped like the code I
                 * meant to write rather than the code that runs.
                 */
                if (ctl_fd >= 0) {
                    FD_SET(ctl_fd, &rfds);
                    if (ctl_fd > nfds)
                        nfds = ctl_fd;
                }
                tv.tv_sec  = 0;
                tv.tv_usec = 250000L;
                /* Between songs: sleep until a byte, a REQUEST or a
                 * signal. While a channel is being waited for: wake
                 * to retry. */
                select(nfds + 1, &rfds, NULL, NULL,
                       said_no_channel ? &tv : NULL);
                /*
                 * NOT `continue' WHEN A REQUEST IS WAITING. The
                 * drain is further down the loop, so continuing
                 * here would go straight back into the select and
                 * the request would wake us for ever without being
                 * read. Fall through instead; the branch below
                 * skips the render because dsp_fd is still < 0.
                 */
                if (ctl_fd >= 0 && FD_ISSET(ctl_fd, &rfds)) {
                    char req[VMIDID_CTL_LINE];

                    while (vmidid_ctl_read(ctl_fd, req, sizeof req) == 1)
                        ctl_command(req, &r, ctl_fd, &max_voices);
                }
                continue;
            }
        }

        /*
         * --- 1a. the control channel -------------------------------
         *
         * DRAINED ON EVERY PASS THAT REACHES HERE, and cheap when
         * empty: one read that returns EAGAIN. Several requests can
         * arrive together - a GUI pressing Apply sends one line per
         * setting - so this loops until the channel is dry rather
         * than taking one per block.
         *
         * "EVERY PASS" IS NOT THE SAME AS "ALWAYS", AND THE
         * DIFFERENCE WAS A BUG. The released-channel branch above
         * ends in `continue', so while the synth is idle this point
         * is never reached - the select there now watches ctl_fd
         * and drains it itself. Two drains, because there are two
         * places the loop can wait.
         *
         * BETWEEN BLOCKS, like the nudge below, so a setting never
         * changes in the middle of rendering one.
         */
        if (ctl_fd >= 0) {
            char req[VMIDID_CTL_LINE];

            while (vmidid_ctl_read(ctl_fd, req, sizeof req) == 1)
                ctl_command(req, &r, ctl_fd, &max_voices);
        }

        /*
         * --- 1b. a voice-ceiling nudge, if a signal asked ----------
         *
         * BETWEEN BLOCKS, which is where render_set_max_voices() says
         * to call it: "lowering it mid-render does not stop voices
         * already sounding, it only stops new ones being allocated
         * above the new limit". So a lowered ceiling takes effect as
         * voices free rather than by cutting anything off - no click,
         * and the piece keeps playing.
         *
         * READ AND CLEARED IN ONE STEP so a signal arriving during
         * this is not lost: the local copy is what gets applied, and
         * anything that lands afterwards is still pending for the
         * next block.
         */
        /*
         * A NUDGE ENDS AUTOMATIC REDUCTION. design/21: "a user who
         * signals a value should not have it moved back". Said once,
         * so a capture shows when it stopped. (`set voices' on the
         * control channel sets the ceiling instead - see there.)
         */
        if (voice_nudge != 0) {
            if (autovoice_manual(&g_av))
                fprintf(stderr, "vmidid: automatic voice reduction off"
                                " - the ceiling was set by hand\n");
        }

        if (voice_nudge != 0) {
            int nudge = (int) voice_nudge;
            int want;

            voice_nudge = 0;

            /* STEPS OF 8. One voice at a time would need dozens of
             * signals to matter; the whole useful range is 16 to 64,
             * so eight is three presses from one end to the other. */
            want = r.max_voices + nudge * 8;

            /*
             * CLAMPED HERE, NOT LEFT TO render_set_max_voices(),
             * so the "did anything change" test below sees the real
             * destination. Without this, signalling up at the ceiling
             * computes 72, compares 72 != 64, and reports a change
             * that did not happen - once per signal.
             */
            if (want > VMIDID_MAX_VOICES)
                want = VMIDID_MAX_VOICES;
            if (want < 1)
                want = 1;

            /*
             * AND SNAP BACK TO A MULTIPLE OF 8 ON THE WAY UP. The
             * floor clamp breaks the grid - 8 down from 8 is 1, and
             * every step up from there lands on 9, 17, 25 rather
             * than the numbers it came down through. Rounding up to
             * the next multiple restores it, so up and down retrace
             * the same values.
             */
            if (nudge > 0 && want > 1 && want < VMIDID_MAX_VOICES)
                want = ((want + 7) / 8) * 8;

            if (want != r.max_voices) {
                int got = vmidid_set_voices(&r, want);

                /* SAY IT EVERY TIME, and to stderr where the rest of
                 * the daemon's state goes. Someone signalling a
                 * daemon they cannot see needs the confirmation, and
                 * a capture needs to record that the ceiling moved
                 * mid-run or a later reading of the log is wrong. */
                fprintf(stderr, "vmidid: voices -> %d%s\n", got,
                        got == VMIDID_MAX_VOICES ? " (the maximum)"
                                                 : "");
            }
        }

        /* --- 2. render: one block, or ahead if -q asked for it ------ */
        if (queue_blocks == 0) {
            /* THE FIX. Render the block that is about to be written and
             * nothing else, so a note arriving now is in the next block
             * out, not q blocks later. */
            render_mix(&r, queue, block_frames);
            queued = 1;
        } else {
            while (queued < queue_blocks) {
                render_mix(&r, queue + (size_t) queued * block_frames
                                       * SYNTH_CHANNELS, block_frames);
                queued++;
            }
        }

        /*
         * --- 2b. automatic voice reduction, on TiMidity's model -----
         *
         * design/21 section 16, rebuilt 2026-10-02. THE SIGNAL IS OUR
         * OWN CHANNEL'S FILL, read just before the write: vsound
         * answers GETOSPACE from this channel's ring alone, so it is
         * this synth falling behind and nobody else. (The first
         * version timed render_mix() instead and cut a synth that was
         * keeping up - tests/logs/2026-10-02-86box-autovoice-
         * gmstriving.) A device that cannot answer leaves it idle.
         */
        if (g_av.on && dsp_fd >= 0 && rate > 0) {
            audio_buf_info bi;

            if (ioctl(dsp_fd, SNDCTL_DSP_GETOSPACE, &bi) == 0
                && bi.fragstotal > 0 && bi.fragsize > 0) {
                long total = (long) bi.fragstotal * bi.fragsize;
                long fill  = (total - bi.bytes) * 1000L / total;
                /* IN TWO PARTS so a large -b cannot overflow a 32-bit
                 * long: frames x 1000000 does at 4295 frames. */
                long blk = ((long) block_frames * 1000L / rate) * 1000L
                         + ((long) block_frames * 1000L % rate) * 1000L
                           / rate;
                int  active, held, shed = 0, got;

                render_voice_counts(&r, &active, &held);
                got = autovoice_block(&g_av, fill, blk, active, held, &shed);
                if (shed > 0)
                    shed = render_shed_tails(&r, shed);
                if (got > 0) {
                    render_set_max_voices(&r, got);
                    if (got >= g_av.ceiling)
                        fprintf(stderr, "vmidid: voices -> %d (automatic:"
                                " buffer healthy again)\n", got);
                    else
                        fprintf(stderr, "vmidid: voices -> %d (automatic:"
                                " buffer %ld%% and falling, %d tails"
                                " shed)\n", got, g_av.avg / 10, shed);
                }
            }
        }
        /*
         * SOUND IS A VOICE OR A NON-ZERO SAMPLE - the guard, 2026-10-03.
         * `render_active()' sees voices only, and the reverb rings on
         * 0.9-1.4 s after the last one ends (design/21 section 13), the
         * chorus for its 2048-sample line. So the block about to go
         * out counts as sound while ANY sample in it is not zero: the
         * reverb decays to exact zero in integer arithmetic, so this
         * is a true test, not a threshold. With it a short -R (the
         * user asked for a ChannelRelease key) waits for the tail
         * instead of cutting a song's last chord off mid-decay -
         * RELEASE_MS_DEFAULT's comment asked for exactly this.
         */
        if (render_active(&r) > 0
            || !pcm_silent(queue, (size_t) block_frames * SYNTH_CHANNELS))
            gettimeofday(&last_sound, NULL);

        /* --- 3. write one block, which paces everything ------------- */
        {
            size_t want = (size_t) block_frames * SYNTH_CHANNELS
                          * sizeof(short);

            /* THE WHOLE BLOCK, however many writes it takes - see
             * write_block(). Until 2026-09-14 a short return here was
             * counted and the queue shifted regardless, and the
             * unwritten tail was the music that went missing. */
            if (write_block(dsp_fd, queue, want, &short_writes) < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "vmidid: write: %s\n", strerror(errno));
                break;
            }

            /*
             * SHIFT THE QUEUE DOWN BY ONE BLOCK.
             *
             * A memmove of at most (q-1) blocks, once per block
             * written. At the default 256x8 that is 7 KB per 5.8 ms,
             * which is nothing next to rendering 256 frames through
             * up to 64 voices - and it keeps the queue a plain array
             * that render_mix can write into directly. A ring would
             * save the copy and cost a wrap test in the hot path plus
             * two writes per block at the seam; this is the cheaper
             * trade at these sizes, and if a P3 run says otherwise
             * the array is easy to make circular.
             */
            queued--;
            if (queued > 0)
                memmove(queue, queue + (size_t) block_frames * SYNTH_CHANNELS,
                        (size_t) queued * block_frames * SYNTH_CHANNELS
                        * sizeof(short));
            blocks_out++;
        }

        /* --- follow the output: once a second, ask again ------------ */
        /*
         * design/54 6b step 2, 2026-10-03 - the user: "would an
         * occasional poll looking for dev/vsound if it knows it is not
         * talking to vsound work". Until now the output was resolved
         * only at an open, so a synth on the card under
         * `@VSOUND:<card>@' reached vsound only after its release
         * (design/36 row 136) - never while a piece played, and never
         * when vsoundd needed that very card.
         *
         * vlhe_resolve_out() decides: vsound once vsoundd says READY;
         * WAIT (0) while vsoundd is waiting for this very card; the
         * card otherwise, including when the pump has gone. A literal
         * `-o' path always resolves to itself, so this never moves it.
         * One /proc read and one small file a second.
         *
         * A NEW DEVICE: discard what the old one has queued
         * (SNDCTL_DSP_RESET - a hand-over must not wait for a real
         * card to play its buffer out) and close; the next pass
         * reopens at once, before the no-channel panic, so sounding
         * voices carry over. WAIT: the same close, and notes drop for
         * the moment vsoundd takes the card - about a second, which
         * the user accepted.
         */
        if (ms_since(&last_follow) >= 1000L) {
            char now[sizeof dsp_resolved];
            int  rc;

            gettimeofday(&last_follow, NULL);
            rc = vlhe_resolve_out(dsp_dev, now, sizeof now);
            if (rc == 0 || (rc == 1 && strcmp(now, dsp_resolved) != 0)) {
                if (rc == 0)
                    fprintf(stderr, "vmidid: letting go of %s - vsoundd is"
                                    " waiting for it\n", dsp_resolved);
                else
                    fprintf(stderr, "vmidid: %s -> %s: moving now\n",
                            dsp_resolved, now);
                (void) ioctl(dsp_fd, SNDCTL_DSP_RESET, 0);
                close(dsp_fd);
                dsp_fd = -1;
                queued = 0;
                follow_reopen   = 1;
                said_no_channel = 0;
                continue;
            }
        }

        /* --- the idle panic ----------------------------------------- */
        /*
         * A STREAM THAT STOPS WITH THE PEDAL DOWN LEAVES VOICES HELD
         * BY NOTHING. design/p3-midi-plan.md section 8, and the whole
         * reason render_panic exists.
         *
         * WHAT THE DAEMON CAN ACTUALLY SEE, which is less than the
         * plan assumed:
         *
         *   - SNDCTL_SEQ_RESET does NOT reach us as an event. It is
         *     handled in the kernel, and in SEQ_1 mode seq_reset()
         *     emits 0xfe then CC 123 on all 16 channels as ORDINARY
         *     MIDI BYTES (sequencer.c:1310-1325). So a reset arrives
         *     as a byte stream we already handle - but CC 123 is the
         *     SOFT one that respects the pedal, so it does not clear
         *     a mid-pedal hang on its own. That is the distinction
         *     section 8 warns about getting backwards.
         *   - the sequencer CLOSING does not close our fd either. We
         *     hold /dev/vmidi across sessions, so musserv exiting is
         *     invisible to us except as silence.
         *
         * SO SILENCE IS THE SIGNAL. If nothing has arrived for
         * idle_ms and voices are still sounding, the stream ended
         * without releasing them and render_panic() runs.
         *
         * WHY A TIMEOUT IS SOUND HERE RATHER THAN A HACK: a musical
         * rest is bounded by the tempo and is nothing like 2 seconds
         * of total silence with notes still held - a held note IS the
         * condition, so a piece resting between phrases with nothing
         * sounding never reaches this. It fires only when voices are
         * ringing and the thing that should have stopped them has
         * gone away.
         */
        if (idle_ms > 0 && render_active(&r) > 0 &&
            ms_since(&last_event) > (long) idle_ms) {
            if (verbose)
                fprintf(stderr, "vmidid: %d voices held and %d ms silent"
                                " - all notes off\n",
                        render_active(&r), idle_ms);
            render_panic(&r);
            panics++;
            /* Do not fire again until something new arrives. */
            gettimeofday(&last_event, NULL);
        }

        /* --- release while silent ----------------------------------- */
        /*
         * NOTHING HAS SOUNDED FOR release_ms: let the channel go so a
         * pump running -R can release the card. The queue holds at
         * most a few tens of ms of blocks, all rendered after the last
         * voice ended, so dropping them loses nothing audible - AS
         * LONG AS release_ms OUTLASTS THE REVERB TAIL, which "no voice
         * active" does not see. RELEASE_MS_DEFAULT's comment has the
         * numbers and the fix for a tighter timer. The next MIDI byte
         * reopens - see the top of the loop.
         */
        if (release_ms > 0 && render_active(&r) == 0 &&
            ms_since(&last_sound) > (long) release_ms) {
            close(dsp_fd);
            dsp_fd = -1;
            queued = 0;
            releases++;
            if (verbose)
                /* "AND NO TAIL" since f5cd0fa counts a block with any
                 * non-zero sample as sound - the reverb and chorus tail
                 * holds the channel too (design/36 row 131, design/54
                 * D60). The line said "no voice sounding" until
                 * 2026-10-04; filed logs carry that wording. */
                fprintf(stderr, "vmidid: %d ms with no voice and no effects"
                                " tail - channel released\n", release_ms);

            /*
             * AND THE DEFERRED RATE LANDS HERE - design/43 6b.
             *
             * THIS IS THE ONE MOMENT IT IS SAFE. render_active() was
             * 0 or we would not be here, so no voice holds a step
             * computed from the old rate; the channel is closed, so
             * the next acquire will negotiate the new rate with the
             * device; and the reverb tail render_set_rate()
             * discards has had release_ms - 3000 by default - to
             * decay.
             *
             * `want_rate' MOVES TOO, and that is the point. It is
             * what every later open asks for, so a rate set here
             * survives a rebind to another device rather than being
             * undone by the next negotiation.
             */
            if (pending_rate != 0) {
                int got = render_set_rate(&r, pending_rate);

                if (got == pending_rate) {
                    want_rate = pending_rate;
                    rate      = got;
                    trace_rate = (long) rate;
                    fprintf(stderr, "vmidid: rate is now %d Hz"
                                    " (applied at the release)\n", rate);
                } else {
                    /* render_set_rate refuses out of range and
                     * returns what is in force. ctl_command checked
                     * the bounds already, so this is a belt-and-
                     * braces path rather than an expected one. */
                    fprintf(stderr, "vmidid: could not change rate to"
                                    " %d - still %d\n", pending_rate, got);
                }
                pending_rate = 0;
            }
            continue;
        }

        /*
         * THE PER-SECOND REPORT, WITH DELTAS - and the delta is the
         * point, not the total.
         *
         * WHAT THIS LINE HAS TO ANSWER is "is the loop keeping up",
         * and cumulative counters cannot say. On the first run
         * (2026-09-07) the daemon starved for nine seconds of
         * thirty-five and the console showed only numbers rising more
         * slowly; the stall was recovered afterwards by differencing
         * block counts off a screenshot. So each line now carries
         * what happened IN THAT SECOND.
         *
         * `blk/s' IS THE MEASUREMENT. One second of audio is
         * rate/block_frames blocks - 172 at the 44100/256 default -
         * and `want' prints that beside it, so a reader needs no
         * arithmetic and no knowledge of the block size. 172/172 is
         * healthy; the run above produced seconds of 40/172, which is
         * 232 ms of audio in 1000 ms of wall clock and a card running
         * dry for the rest.
         *
         * STARVED IS NAMED, NOT LEFT TO BE INFERRED. Under 90% of the
         * target for the second gets the word on the line. A capture
         * that has to be interpreted to be read is one that gets read
         * wrong, and this one already was.
         *
         * IT PRINTS DURING SILENCE TOO, deliberately: for a smoke
         * test whose question is "is it alive", a quiet console is
         * indistinguishable from a wedged daemon. A silent second
         * still shows blk/s at target with 0 voices.
         */
        if (verbose && ms_since(&last_report) >= 1000L) {
            int  active = render_active(&r);
            long want   = (long) rate / block_frames;
            long got    = (long) (blocks_out - prev_blocks);

            fprintf(stderr,
                "vmidid: %lu blk (%ld/%ld blk/s%s)  %lu B (+%lu)"
                "  %lu msg (+%lu)  %d voices  %lu stolen  %lu shed"
                "  %lu clip  buf %ld%%\n",
                blocks_out, got, want,
                (got * 10 < want * 9) ? " STARVED" : "",
                bytes_in, bytes_in - prev_bytes,
                parser.messages, parser.messages - prev_msgs,
                active, r.notes_stolen, r.tails_shed, r.clipped,
                g_av.avg / 10);

            prev_blocks = blocks_out;
            prev_bytes  = bytes_in;
            prev_msgs   = parser.messages;
            gettimeofday(&last_report, NULL);
        }

        /*
         * NO SLEEP HERE - design/24 P4 / D30, removed 2026-10-04. A block
         * "SLEEP ONLY WHEN THERE IS NOTHING TO DO" waited here on MIDI
         * when the queue was full. It could not run: step 3's write
         * happens on every pass and empties a slot (an EINTR `continue's
         * to the top first), so the queue is never full at this point,
         * with -q 0 or with -q N. The blocking write is the pacing, as
         * designed; the released-channel select() above is the one that
         * sleeps between songs.
         */
    }

    /*
     * ON THE WAY OUT: SILENCE THE VOICES, THEN DRAIN.
     *
     * render_panic first, so nothing is left held by a pedal that
     * will never lift; then play out what is already queued so the
     * release tails are heard rather than truncated. A bare close
     * would cut 64 voices mid-envelope, which clicks.
     */
    if (render_active(&r) > 0) {
        render_panic(&r);
        panics++;
    }
    {
        /*
         * DRAIN BOUNDED BY BLOCKS, NOT BY SILENCE. A voice whose
         * release is long - the SC-55 has 3.6 s ones - would hold
         * this open for seconds, and a soundfont with a pathological
         * release could hold it for ever. DRAIN_BLOCKS is the tail
         * that was committed under the old queue, kept as the bound
         * now that -q defaults to 0.
         */
        int left = dsp_fd >= 0 ? DRAIN_BLOCKS : 0;   /* released: no tail */

        while (left-- > 0 && render_active(&r) > 0) {
            render_mix(&r, queue, block_frames);
            /* Whole blocks here too: a short return on the tail used to
             * drop its remainder just as the main loop's did. */
            if (write_block(dsp_fd, queue, (size_t) block_frames
                                           * SYNTH_CHANNELS * sizeof(short),
                            &short_writes) < 0)
                break;
            blocks_out++;
        }
    }

    if (verbose || parser.ignored > 0) {
        fprintf(stderr,
            "vmidid: %lu blocks out (%lu ms), %lu bytes in, %lu messages,"
            " %lu ignored, %lu sysex (%lu dropped)\n",
            blocks_out, frames_to_ms(blocks_out * (unsigned long) block_frames,
                                     rate),
            bytes_in, parser.messages, parser.ignored,
            parser.sysex_msgs, parser.sysex_dropped);
        fprintf(stderr,
            "vmidid: %lu notes, %lu stolen, %lu unmapped, %lu clipped,"
            " %lu panics, %lu short writes\n",
            r.notes_started, r.notes_stolen, r.notes_unmapped, r.clipped,
            panics, short_writes);
        if (g_av.cuts > 0 || r.tails_shed > 0)
            fprintf(stderr,
                "vmidid: automatic voices: %ld cuts, %ld restores, lowest"
                " %d, ended at %d, %lu release tails shed\n", g_av.cuts,
                g_av.restores, g_av.lowest, r.max_voices, r.tails_shed);
        fprintf(stderr,
            "vmidid: %lu channel releases, %lu times notes were dropped"
            " for want of a channel, ended on %s\n",
            releases, dropped,
            dsp_fd < 0 ? "no channel"
                       : have_slot ? "the reserved MIDI slot"
                                  : "an ordinary channel");
    }

    close(midi_fd);
    if (dsp_fd >= 0)
        close(dsp_fd);
    /* THE CHANNEL GOES WITH US. A FIFO left behind would accept a
     * request nobody will ever act on, and a GUI writing to it would
     * wait out its timeout instead of being told at once that the
     * synth is not running. */
    if (ctl_fd >= 0)
        vmidid_ctl_close(ctl_fd);
    free(queue);
    render_free(&r);
    free(pool);

    /* AT A CLEAN EXIT ONLY, so a pid file left behind means the
     * daemon DIED rather than stopped - which is the third state the
     * Status page reports as "stale pid file - it died". */
    vlhe_status_remove_pidfile("vmidid");
    return 0;
}

#endif /* !PARSECHECK */
