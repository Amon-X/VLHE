# defs.mk - the build's defaults, shared by every Makefile in vlhe/.
#
# Copyright (c) 2026 Thomas Tranter
# SPDX-License-Identifier: BSD-3-Clause
#
# Part of VLHE. See LICENSE.TXT for the full license text.
#
# THE NATIVE BUILD IS THE DEFAULT (design/46 section 4). A user on
# Corel 1.2 - or any 2.2 system - builds with the compiler it already
# has: plain gcc, its gtk-config, the kernel source at /usr/src/linux.
# Nothing here knows about any workstation plumbing.
#
# The including Makefile sets VLHE_TOP to the path of vlhe/ from where
# it is, then includes this.
#
# THE ORDER IS THE MECHANISM: defaults first, then the files that
# override them, then what must see the final values.
#
#   1. the defaults - plain `=' assignments
#   2. host/host.mk - the workstation's settings (absent on a target,
#      and the dash makes the include a no-op)
#   3. config.mk    - what ./configure wrote; read last, so it wins
#                     even over host.mk
#   4. what tests the result as the file is read (the MODVERSIONS ifeq)
#
# A later assignment replaces an earlier one, and a value on the
# command line (`make prefix=/usr', which make deb uses) beats every
# assignment here. `=' expands only when a value is used, so
# `bindir = $(prefix)/bin' follows a prefix that config.mk sets later.
#
# NO `?=' - deliberately. Stock Corel 1.2's make is GNU make 3.77, whose
# `?=' stores its value WITHOUT EXPANDING IT (fixed in 3.78): `x ?=
# $(prefix)/bin' made x the literal text `$(prefix)/bin', which broke a
# stock build. The one thing `?=' would add - a default giving way to an
# environment variable - nothing here uses; set things with configure or
# on the command line. Written for 3.77: `=', ifeq and $(shell) only.

# --- 1. the defaults ----------------------------------------------------

# What to build - configure sets these from what it found; without it,
# everything is tried.
BUILD_MODULES = 1
BUILD_GUI     = 1

# Userspace programs, and the GTK ones (which link GTK 1.2 shared).
TCC     = gcc
GUICC   = gcc
GTKCFG  = gtk-config
# A suffix on every built program - empty here, `.295' on the
# workstation where the staging scripts look for that name.
X       =

# Kernel modules. KERNELDIR must be the tree the RUNNING kernel was
# built from; configure checks that (host/check-kernel.sh's job).
KERNELDIR   = /usr/src/linux
KCC         = gcc
KGCCINC     = $(shell $(KCC) -print-file-name=include)
KPREP       = true
LDR         = ld -r

# MODVERSIONS OFF BY DEFAULT - the shipping configuration (design/39
# section 0: "the release version will not have modversions enabled").
# A kernel built with CONFIG_MODVERSIONS=y needs it on, and configure
# reads that from the RUNNING kernel (/proc/ksyms), not from .config.
MODVERSIONS = 0

# THE VOLUME BOOST CEILING, per cent of unity (100) - 2026-10-02, the
# user: 200, "but should it be able to change when compiled?" So
# `make VOL_BOOST=150'. Passed to the modules AND the tools (INC, in
# the Makefile) so the two cannot disagree about the top of a slider.
VOL_BOOST = 200

# THE SYNTH'S VOICE CAP - 2026-10-02, the user: "cap at 512 default to
# 64", and "for consistency and doing it properly" a build setting like
# VOL_BOOST: `make MAX_VOICES=1024'. The default (64) does not move.
# Passed to vmidid AND the tools (INC) so the daemon, the Midi page and
# the config check agree, and stamped into setup-vlhe at install.
# Between 64 (the default must fit) and 2048 (the renderer's bound);
# vmidid.c refuses to compile outside that.
MAX_VOICES = 512

# Where `make install' puts things. configure may override.
prefix   = /usr/local
bindir   = $(prefix)/bin
sbindir  = $(prefix)/sbin
libdir   = $(prefix)/lib/vlhe
datadir  = $(prefix)/share/vlhe
# Corel's manpath.config searches /usr/local/man for /usr/local/sbin.
mandir   = $(prefix)/man
# Documents, as Corel's packages keep them: /usr/doc/<package>, so a
# source install under /usr/local puts its licence in /usr/local/doc/vlhe.
docdir   = $(prefix)/doc/vlhe
MODDIR   = /lib/modules/$(shell uname -r)/misc
# KDE 1's tree on Corel Linux - the icons and the menu entry go here,
# not under $(prefix): KDE looks nowhere else (design/54 G12).
kdedir   = /usr/X11R6/share
DESTDIR  =

# --- 2. the workstation, 3. configure -----------------------------------

-include $(VLHE_TOP)/host/host.mk

# What ./configure wrote, if it ran. Optional: `make' with no
# configure builds on the defaults above, which packaging relies on.
-include $(VLHE_TOP)/config.mk

# --- 4. derived from the final values ------------------------------------

KSOUNDDIR = $(KERNELDIR)/drivers/sound

ifeq ($(MODVERSIONS),1)
KMODVER  = -DMODVERSIONS -include $(KERNELDIR)/include/linux/modversions.h
else
KMODVER  =
endif

VOLDEF    = -DVSOUND_VOL_BOOST=$(VOL_BOOST)
VOICEDEF  = -DVMIDID_MAX_VOICES=$(MAX_VOICES)

KCFLAGS  = -D__KERNEL__ -DMODULE $(KMODVER) $(VOLDEF) \
           -nostdinc -I$(KERNELDIR)/include -I$(KGCCINC) \
           -O2 -fno-strict-aliasing -fno-common \
           -Wall -Wstrict-prototypes -Wno-unused \
           -Werror-implicit-function-declaration
