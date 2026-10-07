/*
 * vlhe_status.h - what is loaded and what is running.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * Reads /proc/modules and the pid files in /var/run. No ioctls and no
 * device nodes, so it is separable and testable - the same reason
 * vlhe_conf.c is its own file.
 *
 * WHAT `present' MEANS, AND WHAT IT DOES NOT. For a module, loaded.
 * For a daemon, THE PROCESS EXISTS - not that it is working.
 * vlhe_backend.h says so where it defines struct vlhe_component, and
 * design/09 says the counters that would tell a WEDGED daemon from a
 * quiet one do not exist. Nothing here may imply more than "it is
 * there".
 *
 * PID FILES ARE THE PLATFORM'S CONVENTION, measured rather than
 * assumed: design/09's finding 4 counted seven in /var/run on the
 * guest - apmd, crond, fontfs, inetd, klogd, syslogd, xfs - all
 * root-owned, all "the number and a newline, nothing else". One file
 * per PROCESS, not per service, which is why the disc daemons carry
 * their drive number.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_STATUS_H
#define VLHE_STATUS_H

/* For size_t, which vlhe_resolve_out() takes. This header used
 * nothing from libc until then, so the include is new rather than
 * assumed to be there already: a caller that includes this first
 * would otherwise fail to compile. */
#include <stddef.h>

/* Is a module loaded? Reads /proc/modules, matching the FIRST FIELD
 * exactly - so "vsound" does not match "vsoundfoo", and the `.o'
 * suffix is not expected because /proc/modules never carries it
 * whatever /etc/modules was written with.
 *
 * Returns 1, or 0 when it is not loaded OR /proc is unreadable. A
 * machine without /proc reports nothing loaded, which is the same
 * answer a GUI can draw. */
int vlhe_status_module_loaded(const char *name);

/* How many clients a module reports, from /proc/modules' use count.
 * -1 when the module is absent or the field cannot be read. */
int vlhe_status_module_users(const char *name);

/*
 * WHO HOLDS THIS MODULE? Fills `out' with the names 2.2 lists in
 * brackets and returns how many.
 *
 * THIS IS HOW THE USER'S SOUND CARD IS IDENTIFIED, and it is the only
 * method that works for every case. A module cannot be a sound device
 * without calling register_sound_dsp() or one of its siblings, and
 * those are exported BY soundcore (sound_core.c:251-256) - so any
 * driver that provides a /dev/dsp must link against soundcore and
 * must therefore appear in its bracket list.
 *
 * That holds where nothing else does:
 *
 *   legacy ISA (sb)          in /proc/sound AND in the brackets
 *   PCI (es1371, esssolo1)   NOT in /proc/sound - CLAUDE.md section 5,
 *                            they register their own fops and never
 *                            enter audio_devs[] - but in the brackets
 *   out-of-tree              nothing names it anywhere else
 *   hand-built (emu10k1, AWE) same
 *
 * vlhe-probe asks "is es1371 loaded?" for each name it knows, which
 * only ever finds cards we thought to list. Asking soundcore who
 * holds it returns whatever is actually there.
 *
 * IT REPORTS WHAT IS LOADED, NOT WHAT THE HARDWARE IS. A machine with
 * the wrong driver loaded reports the wrong driver confidently. That
 * is the right answer for what we do with it - we sit in front of the
 * driver that is working now - but it is not hardware detection and
 * must not be described as such.
 */
#define VLHE_MAX_HOLDERS 16
int vlhe_status_module_holders(const char *name,
                               char out[][64], int max);

/* Read a pid file. Returns the pid, or 0 if the file is missing,
 * empty or does not parse.
 *
 * DOES NOT CHECK THE PROCESS - see below, because the two failures are
 * different and the caller wants to tell them apart. */
int vlhe_status_read_pidfile(const char *path);

/* Is this pid a live process? kill(pid, 0) - permission is enough,
 * EPERM means it exists and is not ours.
 *
 * GUARDED ON pid > 0, AND THAT IS NOT DECORATION. CLAUDE.md records
 * kill(-1, SIGKILL) taking the user's X session twice on 2026-09-14,
 * from a pid field the code under test had reset to -1. Signal 0
 * sends nothing, but 0 and -1 still mean "my process group" and
 * "everything" to the kernel, so the guard is here as well. */
int vlhe_status_pid_alive(int pid);

/* The two together, with the distinction the Status page needs:
 *
 *    > 0   running, and this is the pid
 *      0   no pid file - never started, or stopped cleanly
 *     -1   A PID FILE NAMING A DEAD PROCESS - it crashed, or was
 *          killed, and left its file behind
 *
 * THE THIRD CASE IS THE INTERESTING ONE and the reason this returns
 * more than a boolean: a stale pid file is evidence of a daemon that
 * died rather than one that was never started, which is exactly what
 * someone looking at the Status page after a failure wants to know. */
int vlhe_status_daemon(const char *pidfile);

/* Where a daemon's pid file is: /var/run/<name>.pid. The disc daemons
 * take a drive number, because vdiscd serves ONE drive (-d N is a
 * single value) so four discs is four processes - design/09's finding
 * 4, following sysklogd, which writes two files for two daemons.
 *
 * Pass drive < 0 for a daemon that has only one instance. Returns a
 * static buffer. */
const char *vlhe_status_pidfile_path(const char *name, int drive);

/* WRITE AND REMOVE THIS PROCESS'S PID FILE - for the daemons, so the
 * Status page can tell running from stopped. Until 2026-09-19 none
 * of them wrote one and the page said "no pid file" for all three
 * while all three were running. Failure is not fatal: a daemon that
 * cannot write /var/run still works, it is only harder to see. */
int  vlhe_status_write_pidfile(const char *name);
void vlhe_status_remove_pidfile(const char *name);


/* The directory pid files live in. Overridable for testing, as the
 * config paths are; a shipped machine uses /var/run. */
const char *vlhe_status_rundir(void);
/* The daemons' own directory - pid files, vsoundd.state: ctl/ under the
 * run directory, the account's (design/55 section 14). VLHE_RUNDIR
 * names one directory for both. */
const char *vlhe_status_ctldir(void);

/*
 * THE PUMP'S STATE - design/54 section 6b, 2026-10-03. vsoundd writes
 * <run dir>/vsoundd.state: its pid, the card it plays through, and
 * whether it is WAITING for that card (busy) or READY - it holds the
 * card and has set vsound's mix rate. A daemon on the card moves to
 * vsound only on READY: vsound refuses a mix-rate change once any
 * channel is open, so arriving earlier would pin the wrong rate.
 *
 * vlhe_pump_state() returns VLHE_PUMP_READY or VLHE_PUMP_WAITING with
 * the card copied into `card' (may be NULL), or -1 when there is no
 * file or the pid in it is not alive - a pump that died left it.
 */
#define VLHE_PUMP_WAITING   0
#define VLHE_PUMP_READY     1
int  vlhe_pump_state_write(const char *card, int ready);   /* 0 / -1 */
void vlhe_pump_state_remove(void);
int  vlhe_pump_state(char *card, size_t max);

/*
 * MAY THIS FAILED CARD OPEN MEAN "BUSY"? 1 for every errno but ENOENT
 * (no node) and ENXIO (the legacy core, no device at that minor). Not
 * a list of busy errnos, because the cards do not agree on one:
 * EBUSY (esssolo1, es1370/1371, cmpci, sonicvibes, vwsnd, the legacy
 * path), EWOULDBLOCK (maestro), and ENODEV from i810 and trident with
 * every channel taken - the same errno soundcore gives for a minor with
 * no driver. design/54 section 6c has the survey.
 */
int vlhe_card_open_retryable(int err);

/* /proc/modules, likewise. */
const char *vlhe_status_modules_path(void);

/*
 * WHICH SEQUENCER MIDI DEVICE vmidi IS, from ITS OWN /proc/vmidi
 * ("midi: N"). -1 when the module is not loaded, or when the file
 * cannot be read - no proc filesystem, or VLHE_PROC_VMIDI points at
 * a fixture without it. design/36 rows 62 and 84.
 *
 * NOT /dev/sequencer: that is single-open and its open resets
 * synths, so a probe from a two-second poll would fight musserv for
 * it.
 *
 * AND NOT /proc/sound ANY MORE, CHANGED 2026-09-26. That was the
 * source until an oops on a guest with three es1371 and `sb' turned
 * out to be the kernel walking `audio_devs[]' past its five entries
 * while building the file we were reading. The number we wanted was
 * one line of it, and vmidi knew it all along. See vlhe_status.c for
 * the mechanism and why there is no fallback.
 */
/*
 * WHICH /dev/dspN IS VSOUND, from /proc/vsound - design/38 9f2.
 *
 * Returns the dsp INDEX (0 for /dev/dsp, 1 for /dev/dsp1 ...) or -1
 * if the file is absent or unreadable, which is what an unloaded
 * module looks like.
 *
 * THIS IS THE ONLY WAY THAT WORKS WHEN THE NODE IS MISSING. On a
 * stock machine `MAKEDEV audio' creates two dsp nodes and an es1371
 * takes both, so vsound can hold a minor with no node to open - and
 * an ioctl needs a node. The module publishes the number instead.
 *
 * VLHE_PROC_VSOUND overrides the path, for the host tests.
 */
int vlhe_vsound_dsp(void);

/*
 * WHERE A DRIVE'S AUDIO IS, FROM /proc/vdisc - design/47 G1, 2026-10-01.
 * The module's own answer (the same the SUBCHNL ioctl gives) with no
 * node opened and no cdrom-layer close behind it. Returns the lba, or
 * -1 when the entry is absent (an older module, or vdisc not loaded)
 * or the drive is not attached. `playing' is 1 for play, 0 otherwise.
 * VLHE_PROC_VDISC overrides the path, for the host tests.
 */
int vlhe_vdisc_position(int index, int *track, int *playing);
/* One integer field of that drive's block (`locked', `attached', ...),
 * or -1 when there is no entry, no such drive, or no such key. */
int vlhe_vdisc_proc_int(int index, const char *key);
/* How many drives the loaded module advertises - the `drive:' blocks
 * in /proc/vdisc - or -1 when there is no entry (not loaded, or a
 * module older than the entry). design/54 D20. */
int vlhe_vdisc_proc_drives(void);
/* Does the loaded vdisc have the CD-ROM packet interface (ripping,
 * Video CD)? 1 or 0 from /proc/vdisc's module-wide `packet_interface'
 * line, -1 when unknown: no entry (vdisc not loaded) or a module older
 * than the line. A kernel before 2.2.16 has no such interface (design/39
 * section 3h). */
int vlhe_vdisc_packet_interface(void);

/*
 * RESOLVE A DAEMON'S `-o' AT OPEN TIME - design/43 part A.
 *
 * `spec' is either the literal token VLHE_OUT_VSOUND ("@VSOUND@"),
 * meaning "wherever vsound is NOW", or an ordinary path which is
 * copied through unchanged.
 *
 *   1   resolved; `out' holds the path
 *   0   the token was given and vsound is NOT USABLE - not loaded, or
 *       loaded with its pump (vsoundd) not READY (design/54 6b,
 *       2026-10-03). `out' is untouched - THE CALLER MUST NOT
 *       SUBSTITUTE ANYTHING. Wait and ask again.
 *  -1   bad arguments, or the answer did not fit
 *
 * WHY 0 IS NOT AN ERROR AND NOT A PATH. The plan emits the token
 * only when it is also going to load vsound, so "not yet" is a
 * normal state during a load and the answer is to retry, not to
 * fall back. Falling back to a real card is a DIFFERENT MODE - the
 * release cycle that is correct on vsound's reserved slot hands a
 * real card to whoever asks next (design/36 row 53), and a daemon
 * that did it silently would sound intermittently broken with no
 * line saying why.
 *
 * SO THE DAEMONS NEVER GUESS. When playing straight to a card is
 * what is wanted, the PLAN says so by emitting the card's own path,
 * which arrives here as an ordinary string.
 */
#define VLHE_OUT_VSOUND "@VSOUND@"

/*
 * "VSOUND IF IT IS THERE, THIS CARD IF IT IS NOT" - the third form,
 * added 2026-09-26 after the first target run of the rebind showed
 * why two were not enough.
 *
 *     @VSOUND:/dev/dsp1@
 *
 * WHAT IT FIXES. `@VSOUND@' and a literal card path each foreclose
 * half of what is wanted: the token waits forever when the user
 * never intends to load vsound, and a literal path can never move
 * when vsound arrives later. The run of 2026-09-26 18:15 loaded MIDI
 * and CD first with Sound unticked, so the plan emitted the card -
 * correctly, for that moment - and then vsound came up at 18:19 and
 * NEITHER DAEMON MOVED. `vsoundd: stopping, 0 bytes written`.
 *
 * SO THE CHOICE IS THE DAEMON'S, MADE FRESH AT EVERY OPEN, and the
 * plan supplies both answers rather than picking one at build time.
 * Resolves to vsound when /proc/vsound exists AND vsoundd says READY
 * (design/54 6b, 2026-10-03), to the embedded path otherwise - and it
 * re-checks on every acquire, so a vsound loaded or unloaded
 * mid-session is followed either way. ONE EXCEPTION RETURNS 0 (wait):
 * vsoundd WAITING for the very card embedded here, which the daemon
 * asking is likely holding - it must let go, not take it back.
 *
 * THE CARD PATH IS EMBEDDED BECAUSE THE DAEMON CANNOT WORK IT OUT.
 * probe_card() reads the user's `CardDevice' pick and the session
 * note, neither of which a daemon has; the plan knows and passes it.
 */
#define VLHE_OUT_VSOUND_OR "@VSOUND:"

/* A buffer big enough for any answer. The longest this can produce
 * is "/dev/dsp7", but `spec' may be an arbitrary path from the plan,
 * so this matches VLHE_PATH_MAX's order rather than the device
 * names'. */
#define VLHE_OUT_MAX 256

int vlhe_resolve_out(const char *spec, char *out, size_t max);

int vlhe_midi_seqdev(void);

#endif /* VLHE_STATUS_H */
