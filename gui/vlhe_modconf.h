/*
 * vlhe_modconf.h - would opening a sound device node LOAD A MODULE?
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * WHY, 2026-10-04 (CLAUDE.md section 5, corrected that day). On 2.2
 * with CONFIG_KMOD - both kernels here - opening a sound node is not
 * free:
 *
 *  - with soundcore not loaded, chrdev_open() requests
 *    `char-major-14' (fs/devices.c:94), and modutils 2.1.121 carries a
 *    BUILT-IN alias for it (to `sound', the usual modutils default);
 *  - with soundcore loaded but nothing at the minor, soundcore_open()
 *    requests `sound-slot-N' and `sound-service-N-M'
 *    (sound_core.c:366-369) before answering ENODEV - and an owner's
 *    `alias sound-slot-0 sb' then loads that driver.
 *
 * So a probe that opens a node only TO LOOK can load a module: a change
 * nothing records, which also changes the answer. Look-only probes ask
 * this first. A program that USES the device (vsoundd opening the card)
 * does not - an alias loading the card's driver then is the owner's
 * configuration doing what it is for.
 *
 * ONLY A RISK FOR AN EMPTY MINOR: a registered unit is found before any
 * request_module(). So an alias whose module is already loaded is safe,
 * and `alias sound-slot-0 sb' with sb loaded does not hide the card.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_MODCONF_H
#define VLHE_MODCONF_H

#include <stddef.h>

/* 1 when opening the sound-major node with this MINOR could load a
 * module, with `why' (may be NULL) naming the alias; 0 when it cannot.
 * Reads the effective configuration with `modprobe -c' (falling back to
 * /etc/conf.modules and /etc/modules.conf plus modprobe's built-in
 * char-major-14), cached for a few seconds. VLHE_MODCONF names a file
 * to read instead (the host tests); VLHE_PROC_MODULES is honoured as
 * everywhere. */
int vlhe_modconf_open_would_load(int minor, char *why, size_t max);

#endif
