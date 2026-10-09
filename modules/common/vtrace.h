/*
 * vtrace.h - a module's trace: a ring of text lines read through
 * /proc/<module>-trace. The interface; vtrace.c is the implementation,
 * included ONCE per module.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/54 section 8 as revised by 8f, 2026-10-08 - tracing without
 * taking over syslog. Every KERN_DEBUG printk a module used to make
 * goes to its own ring instead, and `cat /proc/vsound-trace' (or `vlhe
 * trace', which merges the three modules' files by timestamp) reads it.
 * The kernel log keeps errors and one-time events only - THE RING IS
 * THE ONLY PLACE A TRACE LINE GOES. A kernel-log path was kept beside
 * it for one day (a <mod>_trace_to parameter: kmsg, ring, both) so the
 * two could be compared on the same run; the comparison was made (the
 * kernel log lost 195 lines the ring kept, design/36 row 188) and the
 * user then removed it, 2026-10-08: "They shouldnt be in there at all.
 * printk for errors and the one-time lines ... No ... tracing via
 * printk." So nothing here calls printk.
 *
 * ONE SOURCE, THREE COPIES, THREE NAMES. The three modules load and
 * unload independently and must not share a symbol, so each includes
 * vtrace.c once with VTRACE_MOD set to its name, and every function
 * here is spelled <mod>_vt_<name> - vsound_vt_printf, vmidi_vt_printf
 * - through the VT() macro. insmod exports a module's globals, and two
 * modules exporting the same name is a collision nobody needs.
 *
 * USE:
 *     #define VTRACE_MOD vsound
 *     #include "../common/vtrace.h"       (every file of the module)
 *     #include "../common/vtrace.c"       (one file of the module)
 *
 *     VT(start)("vsound", 2048);          at init, when tracing is on
 *     VT(printf)("open pid %d", pid);     the former printk(KERN_DEBUG
 *     VT(stop)();                         at cleanup
 *
 * The line gets "<seq> <sec>.<usec> vsound: " in front; the caller
 * writes the message only. With no start(), printf() does nothing.
 */
#ifndef VTRACE_H
#define VTRACE_H

#ifndef VTRACE_MOD
#error "define VTRACE_MOD (the module's name, bare) before vtrace.h"
#endif

#define VTRACE_CAT2(a, b)  a##b
#define VTRACE_CAT(a, b)   VTRACE_CAT2(a, b)
#define VT(n)              VTRACE_CAT(VTRACE_MOD, VTRACE_CAT(_vt_, n))

/* Tracing on: the ring vmalloc'd and the proc file made. 0, or -ENOMEM
 * / -ENOENT (then nothing is on). `nslots' lines of VTRACE_SLOT (96)
 * bytes - 2048 is 192 KB. */
int  VT(start)(const char *name, unsigned long nslots);

/* Off: the proc file removed, the ring freed. Safe when never started. */
void VT(stop)(void);

/* One line, into the ring. Formatted under the ring's lock into a
 * bounded buffer; safe from a timer or an interrupt. The compiler
 * checks the format. Nothing until start(). */
void VT(printf)(const char *fmt, ...)
        __attribute__((format(printf, 1, 2)));

#endif /* VTRACE_H */
