# vsound / vdisc - the rewrite. See design/07-vsound.md.
#
# Copyright (c) 2026 Thomas Tranter
# SPDX-License-Identifier: BSD-3-Clause
#
# Part of VLHE. See LICENSE for the full license text.
#
# SEPARATE FROM vcd/ ON PURPOSE. The old tree still builds, still works
# and is still testable; this one collides with it by design (07's
# section 8), so the two are kept apart rather than policed by an
# exclusion list that would drift.
#
#   make                  userspace tools + host tests
#   make check            run the host tests
#   make module           kernel modules (needs a configured kernel tree)
#   make check-all        everything, in the order that fails fastest
#
# KERNELDIR must point at the tree the RUNNING kernel was built from.
# It is in this repo: usr/src/linux, 2.2.16-usb. See CLAUDE.md section 5.

# THE DEFAULTS ARE THE NATIVE BUILD - defs.mk, shared by every
# Makefile here. On the workstation host/host.mk (read by defs.mk)
# turns the same rules into the cross build. design/46 sections 4-5.
VLHE_TOP  = .
include defs.mk

# EVERYTHING THE BUILD MAKES GOES IN build/ - programs, modules,
# objects and the host tests (build/tests/). The user, 2026-10-02:
# "the vlhe folder is now full of the test binaries. Shouldnt we have
# a build folder they get built into?" `make clean' removes it.
B         = build
DUMMY    := $(shell mkdir -p $(B)/tests $(B)/obj/modules/vsound $(B)/obj/modules/vdisc)

# CC is the HOST compiler - the host tests run where they are built.
# The programs that ship are built with $(TCC) and $(GUICC).
CC        = gcc
INSTALL   = install

# STATIC - vsoundd, vdiscd, vlhe and sysinfo-vlhe link statically, so
# they run before anything is installed and on a machine with nothing
# but the folder they came in. 2026-10-05: they were static only on the
# workstation, where host/corelcc links statically by default; a native
# build on the Acer, with plain gcc, made all four dynamic. Said here
# so both builds agree. (The synth sets its own -static; the control
# centre is dynamic by necessity - GTK 1.2 exists only as shared
# libraries.)
STATIC    = -static

# --- sources ---------------------------------------------------------
# C89 only: the target compiler is GCC 2.95.2. No // comments, no
# declarations after statements, no long long in wire structures.
#
# One prefix per file, following the reference (07's section 7): the
# file a function lives in is predictable from its name.

CFLAGS    = -O2 -Wall -Wstrict-prototypes

# THE TREE SINCE 2026-10-02 (design/46): the shared contracts in
# include/, what the daemons and the GUI both use in lib/, and each
# program beside its own files. Quoted includes still resolve by name.
INC       = -Iinclude -Ilib -Igui -Idaemons/vdiscd -Idaemons/vmidid \
            -Idaemons/vmidid/sf2 -Imodules/vsound -Imodules/vdisc \
            $(VOLDEF) $(VOICEDEF)

# WHAT `make' BUILDS: everything that ships.
# THE SETUID BUILD IS NOT HERE - design/55 13f, 2026-10-04: it is the
# user's test tool, a PORTABLE build, made only by `make portable
# SETUID=1' (below). Never installed setuid by this Makefile.
ifeq ($(BUILD_GUI),1)
GUIPROGS = $(B)/vlhe.gtk$(X)
else
GUIPROGS =
endif
PROGRAMS = $(B)/vsoundd$(X) $(B)/vdiscd$(X) $(B)/vlhe$(X) $(B)/sysinfo-vlhe$(X) $(GUIPROGS)

ifeq ($(BUILD_MODULES),1)
all: programs modules
else
all: programs
endif

programs: $(PROGRAMS) synth $(B)/setup-vlhe

# ---- TWO BUILDS - design/55 13f, the user's decision of 2026-10-04 -----
#
# THE MODE IS CHOSEN HERE, NOT BY THE `PORTABLE' FILE. `make' builds the
# INSTALLED programs: they use the install prefix, /lib/modules and the
# system's config and state, and never anything beside themselves.
# `make portable' builds the PORTABLE ones into $(PB): they use only their
# own folder - modules/, daemons/, tools/ - and never the system's. The
# daemons, smf2wav and the modules are the same in both; only the three
# programs that decide paths differ.
#
# THE SETUID BUILD IS OPT-IN AND DEVELOPMENT-ONLY: `make portable
# SETUID=1', defined in host/dev.mk (the user's test tool, switching to
# root without quitting the GUI; not shipped - design/33 section 3,
# design/55 R6-R8).
#
# INSTDEF tells the installed programs where `make install' puts the
# daemons, which is how they find them now (no "beside this binary").
PB      = $(B)/portable
INSTDEF = -DVLHE_SBINDIR='"$(sbindir)"' -DVLHE_DATADIR='"$(datadir)"'

# MODREL - the kernel MODDIR belongs to, for `depmod -a <kernel>'. An
# installed VLHE loads its modules with modprobe (2026-10-05), which
# reads that kernel's modules.dep, so depmod must run for THAT kernel -
# not the running one, if configure was pointed at another tree.
# /lib/modules/2.2.16-usb/misc -> 2.2.16-usb.
MODREL  = $(notdir $(patsubst %/misc,%,$(MODDIR)))

# GUIDEF - what the control centre's About box says about itself
# (design/54 G01): the version, and the build stamp in the form
# stagelib.sh's vlhe_stamp() gives a staged file - when, and from which
# commit, `-dirty' when the tree had uncommitted work. Taken when make
# runs, so it names the tree the binary was LAST BUILT from; a binary
# make thought up to date keeps the stamp it was built with.
#
# THREE DEFINES, NOT ONE, BECAUSE host/corelcc RE-SPLITS ITS ARGUMENTS:
# a space inside one -D value reached gcc as two words and the build
# failed ("cannot specify -o with -c ... and multiple compilations").
# gui/vlhe_help.c joins them with spaces.
BUILD_DATE   := $(shell date '+%Y-%m-%d')
BUILD_TIME   := $(shell date '+%H:%M')
# A COMMIT FILE FIRST - the source tarball carries one (host/mkdist.sh)
# and has no git; without it a tarball build read `no-git' and then
# `-dirty' too, because a FAILING `git diff' was taken as dirty (seen
# on the Soyo, 2026-10-08). `-dirty' is asked only where git answers.
BUILD_COMMIT := $(shell if [ -f COMMIT ]; then cat COMMIT; \
    elif git rev-parse --short HEAD >/dev/null 2>&1; then \
        echo `git rev-parse --short HEAD``git diff --quiet HEAD 2>/dev/null || echo -dirty`; \
    else echo no-git; fi)
GUIDEF  = -DVLHE_VERSION='"$(shell cat VERSION 2>/dev/null)"' \
          -DVLHE_BUILD_DATE='"$(BUILD_DATE)"' -DVLHE_BUILD_TIME='"$(BUILD_TIME)"' \
          -DVLHE_BUILD_COMMIT='"$(BUILD_COMMIT)"'
PORTABLE_PROGS = $(PB)/vlhe$(X) $(PB)/sysinfo-vlhe$(X)
ifeq ($(BUILD_GUI),1)
PORTABLE_PROGS += $(PB)/vlhe.gtk$(X)
endif

portable: $(PORTABLE_PROGS) $(B)/vsoundd$(X) $(B)/vdiscd$(X) synth $(B)/setup-vlhe
	@echo "portable build in $(PB)/ (the daemons, smf2wav and modules are $(B)/'s)"
	@echo "make portable-dir PORTABLE_DIR=<folder> lays it out to run in place."

# portable-dir - A RUNNABLE PORTABLE FOLDER FROM A SOURCE BUILD - 2026-10-05.
#
# `make portable' compiles the three programs that decide paths; the
# folder they run in was assembled only by host/staging/mkbundle.sh,
# which the source tarball does not have - so a user who downloaded the
# source could build a portable VLHE and not lay it out. This is that
# layout, the one fixed by design/55 13f (the programs look in exactly
# these folders beside themselves):
#
#   ./        vlhe.gtk vlhe setup-vlhe (+ its menu and strings files)
#             vlhe-help.txt, the docs, PORTABLE, README.PORTABLE
#   modules/  vsound.o vdisc.o vmidi.o      (when modules were built)
#   daemons/  vsoundd vdiscd vmidid
#   tools/    smf2wav sysinfo-vlhe
#
# mkbundle.sh builds its tarball FROM THIS, adding only what is its own
# (tools/ide-dma, the build stamp, the -t test tools), so the layout has
# one definition.
#
# IT DELETES NOTHING, and it refuses a non-empty directory that is not
# already a portable folder (no PORTABLE file), so a mistyped
# PORTABLE_DIR cannot scatter files into someone's home.
PORTABLE_DIR = vlhe-$(shell cat VERSION 2>/dev/null)-portable

portable-dir: portable
	@if [ -d "$(PORTABLE_DIR)" ] && [ -n "`ls -A '$(PORTABLE_DIR)' 2>/dev/null`" ] \
	    && [ ! -f "$(PORTABLE_DIR)/PORTABLE" ]; then \
	    echo "$(PORTABLE_DIR) is not empty and is not a portable folder -"; \
	    echo "  refusing to write into it. Name another PORTABLE_DIR=."; \
	    exit 1; \
	fi
	$(INSTALL) -d $(PORTABLE_DIR)/modules $(PORTABLE_DIR)/daemons $(PORTABLE_DIR)/tools
	$(INSTALL) -m 755 $(PB)/vlhe$(X) $(PORTABLE_DIR)/vlhe
	@if [ "$(BUILD_GUI)" = 1 ]; then \
	    $(INSTALL) -m 755 $(PB)/vlhe.gtk$(X) $(PORTABLE_DIR)/vlhe.gtk; \
	else \
	    echo "  no control centre - built without GTK 1.2 (configure)"; \
	fi
	$(INSTALL) -m 755 $(B)/setup-vlhe $(PORTABLE_DIR)/setup-vlhe
	$(INSTALL) -m 644 scripts/setup-vlhe-menu scripts/setup-vlhe-strings \
	    $(PORTABLE_DIR)
	$(INSTALL) -m 755 $(B)/vsoundd$(X) $(PORTABLE_DIR)/daemons/vsoundd
	$(INSTALL) -m 755 $(B)/vdiscd$(X) $(PORTABLE_DIR)/daemons/vdiscd
	$(INSTALL) -m 755 $(B)/vmidid $(PORTABLE_DIR)/daemons/vmidid
	$(INSTALL) -m 755 $(B)/smf2wav $(PORTABLE_DIR)/tools/smf2wav
	$(INSTALL) -m 755 $(PB)/sysinfo-vlhe$(X) $(PORTABLE_DIR)/tools/sysinfo-vlhe
	@if [ -f $(B)/vsound.o ] && [ -f $(B)/vdisc.o ] && [ -f $(B)/vmidi.o ]; then \
	    $(INSTALL) -m 644 $(B)/vsound.o $(B)/vdisc.o $(B)/vmidi.o \
	        $(PORTABLE_DIR)/modules && \
	    echo "  modules/ for kernel $(MODREL)"; \
	else \
	    echo "  NO MODULES - make modules first (configure needs the kernel"; \
	    echo "  source); the folder cannot Load without them."; \
	fi
	$(INSTALL) -m 644 README INSTALL LICENSE ACKNOWLEDGMENTS VERSION \
	    $(PORTABLE_DIR)
	$(INSTALL) -m 644 doc/vlhe-help.txt $(PORTABLE_DIR)/vlhe-help.txt
	sed -e 's|@VERSION@|$(shell cat VERSION 2>/dev/null)|' -e 's|@KREL@|$(MODREL)|' \
	    doc/README.PORTABLE > $(PORTABLE_DIR)/README.PORTABLE
	@printf '%s\n' \
	    "# VLHE portable folder - run in place, nothing installed." \
	    "#" \
	    "# A LABEL. The programs here were built portable (make portable): they" \
	    "# keep everything in this folder - modules/, daemons/, tools/, their" \
	    "# settings - and never use the system's, with or without this file." \
	    "# Deleting it changes nothing." \
	    "#" \
	    "# Modules for kernel $(MODREL) - build from source for any other kernel." \
	    > $(PORTABLE_DIR)/PORTABLE
	@echo "portable folder: $(PORTABLE_DIR)  - run ./vlhe.gtk there"

# The synth daemon and the offline renderer live in their own
# directory with their own Makefile (which includes defs.mk too).
synth:
	@$(MAKE) --no-print-directory -C daemons/vmidid vmidid smf2wav

# The kernel modules. Need the configured kernel tree at KERNELDIR.
modules: module-cross disc-cross
	@$(MAKE) --no-print-directory -C modules/vmidi module

# The pump, cross-built for the target.
# lib/vlhe_status.c FOR THE PID FILE ONLY. The Status page cannot tell a
# running daemon from a stopped one without it (2026-09-19), and the
# path logic already lives there - duplicating it in three daemons is
# how the two halves came to look in different directories.
$(B)/vsoundd$(X): daemons/vsoundd/vsoundd.c lib/vlhe_status.c include/vsound.h include/vlhe_status.h
	$(TCC) $(STATIC) -O2 -Wall -I. $(INC) -o $@ daemons/vsoundd/vsoundd.c lib/vlhe_status.c

# The disc daemon. More sources than the others because the image
# backends come with it - those are the one part of the old tree
# section 7 names as safe to carry across, and they are unchanged
# apart from the data_start fix in lib/image.c.
DISCD_SRCS = daemons/vdiscd/vdiscd.c daemons/vdiscd/vdiscd_play.c daemons/vdiscd/vdiscd_child.c daemons/vdiscd/vdiscd_audio.c \
             daemons/vdiscd/vdiscd_state.c lib/vdiscd_ctl.c lib/vlhe_status.c \
             lib/image.c lib/backend_ccd.c lib/backend_cue.c lib/backend_iso.c lib/msf.c

$(B)/vdiscd$(X): $(DISCD_SRCS) include/vdisc.h lib/image.h daemons/vdiscd/vdiscd_child.h daemons/vdiscd/vdiscd_play.h \
            daemons/vdiscd/vdiscd_state.h include/vdiscd_ctl.h include/vlhe_status.h
	$(TCC) $(STATIC) -O2 -Wall -I. $(INC) -o $@ $(DISCD_SRCS)

# vsoundvol AND vsoundvol-gtk ARE GONE - the user, 2026-10-02, with the
# move to this tree: "both vsoundvol and vsoundvol-gtk do not need."
# The Volume page is the mixer front end; their sources stayed behind
# in the old vsound/ directory and are not built.

# GTK builds are DYNAMIC, unlike everything else here: they link
# against the target's own GTK 1.2, which is only present as shared
# libraries. host/gtkcfg supplies the flags rebased onto the Corel
# tree.

# The control centre. Same dynamic GTK link as vsoundvol-gtk above.
#
# TWO BINARIES FROM ONE SOURCE, differing only in which backend is
# linked. gui/vlhe_cc.c never learns which it got - see gui/vlhe_backend.h.
#
#   $(B)/vlhe.gtk$(X)        the real one. THIS IS WHAT SHIPS.
#   $(B)/vlhe.gtk.fake$(X)   scripted state, for looking at the window here.
#
# The fake one is not staged by mkbundle.sh and not installed. It exists
# so a layout change costs seconds rather than a boot-and-carry cycle.
# gui/vlhe_apply.c IS HERE BECAUSE gui/vlhe_cli.c IS. The one binary answers
# both `vlhe' and `vlhe apply', so the GUI build carries the plan
# builder whether or not a window ever calls it - and design/33
# section 3h wants a Load button that does, through the same code.
CC_SRCS = gui/vlhe_cc.c gui/vlhe_mod_volume.c gui/vlhe_mod_cd.c gui/vlhe_mod_cdg.c \
          gui/vlhe_mod_midi.c gui/vlhe_mod_advanced.c \
          gui/vlhe_mod_status.c gui/vlhe_mod_sound.c gui/vlhe_mod_render.c gui/vlhe_filter.c gui/vlhe_buttons.c gui/vlhe_layout.c gui/vlhe_state.c gui/vlhe_tip.c gui/vlhe_match.c gui/vlhe_priv.c gui/vlhe_picker.c \
          gui/vlhe_cli.c gui/vlhe_apply.c \
          gui/vlhe_journal.c gui/vlhe_help.c gui/vlhe_helptext.c

# gui/vlhe_restart.c - the per-daemon Restart, in its own file so the FAKE
# GUI does not get it: host/vlhe_backend_fake.c has its own scripted
# vlhe_restart() and two definitions in one link is an error. Added to
# the two REAL GUI builds and the CLI by name, never to CC_SRCS.
VLHE_RESTART_SRC = gui/vlhe_restart.c

# $(B)/vlhe.gtk$(X) - THE REAL ONE. Same GUI sources, the real backend and
# the five pieces behind it. THIS IS WHAT SHIPS.
# lib/vdiscd_ctl.c IS IN THE BACKEND because vlhe_attach()/vlhe_detach()
# reach the daemon through it (design/33 section 3f) - the GUI cannot
# attach a disc itself. It is the transport only; nothing here opens
# /dev/vdiscctl.
# lib/vmidid_ctl.c IS THE SYNTH'S CONTROL CHANNEL, and it
# is linked HERE because the caller's half lives in it - gui/vlhe_backend.c
# calls vmidid_ctl_request() for the Status page's "Apply settings"
# button (design/43 6b). Same arrangement as lib/vdiscd_ctl.c beside it:
# one file, both ends, linked by whoever needs either.
BACKEND_SRCS = gui/vlhe_backend.c gui/vlhe_conf.c gui/vlhe_conf_template.c \
               gui/vlhe_fontscan.c gui/vlhe_mixer.c lib/vlhe_status.c \
               gui/vlhe_self.c gui/vlhe_session.c gui/vlhe_baseline.c \
               gui/vlhe_modconf.c lib/vdiscd_ctl.c lib/vmidid_ctl.c lib/cdg.c lib/cdtext.c \
               lib/cddb.c \
               lib/image.c lib/backend_ccd.c lib/backend_cue.c lib/backend_iso.c lib/msf.c \
               daemons/vmidid/autovoice.c daemons/vdiscd/vdiscd_state.c

# THE IMAGE LAYER IS IN THE GUI SINCE 2026-09-26, and it is the cost
# of design/34 6e2: the CD+G viewer opens the attached image READ
# ONLY and decodes the subchannel itself, because the daemon cannot
# answer where playback has reached (its CDDA child reports straight
# to the kernel) and nothing should carry 96 binary bytes a sector
# across a process boundary.
#
# MEASURED RATHER THAN FEARED: 287153 -> 305935 bytes, 18.8 KB and
# 6.5%. It was flagged as "~2000 lines into the GUI binary" before
# anyone linked it, which overstated a cost that does not matter on
# a 16 MB machine.
#
# IT IS IN BACKEND_SRCS so both GUI builds get it - the fake one
# needs it too, since gui/vlhe_mod_cdg.c is shared and calls
# vdisc_image_*() directly.

# gui/vlhe_strings.h IS A PREREQUISITE OF ALL FOUR GUI BINARIES - 2026-10-03.
# Every page's words live there, and without it a strings-only edit left
# the binaries "up to date": run-gui-cc showed the old text, and the
# staged vlhe.gtk could be stale while mkxfer.sh's "matches the local
# build" check still passed, since it compares against this file.
# gui/vlhe_rates.h beside it, 2026-10-04: the synth's sample rates, read
# by Midi Settings, the Render page and the backend's check - the same
# stale-binary trap if it changes alone.
$(B)/vlhe.gtk$(X): $(CC_SRCS) $(VLHE_RESTART_SRC) $(BACKEND_SRCS) gui/vlhe_backend.h \
             gui/vlhe_strings.h gui/vlhe_rates.h \
             gui/vlhe_conf.h gui/vlhe_conf_template.h gui/vlhe_fontscan.h \
             gui/vlhe_mixer.h include/vlhe_status.h include/vsound.h \
             gui/vlhe_mod_volume.h gui/vlhe_mod_cd.h \
             gui/vlhe_mod_midi.h gui/vlhe_mod_status.h \
             gui/vlhe_mod_sound.h gui/vlhe_mod_advanced.h gui/vlhe_icons.h \
             gui/vlhe_help.h gui/vlhe_helptext.h VERSION
	$(GUICC) -O2 -Wall -I. $(INC) $(INSTDEF) $(GUIDEF) -o $@ \
	    $(CC_SRCS) $(VLHE_RESTART_SRC) $(BACKEND_SRCS) \
	    `$(GTKCFG) --cflags` `$(GTKCFG) --libs`

# $(PB)/vlhe.gtk$(X) - THE PORTABLE CONTROL CENTRE (design/55 13f): the
# same sources, compiled to use only its own folder.
$(PB)/vlhe.gtk$(X): $(CC_SRCS) $(VLHE_RESTART_SRC) $(BACKEND_SRCS) gui/vlhe_backend.h \
             gui/vlhe_strings.h gui/vlhe_rates.h \
             gui/vlhe_conf.h gui/vlhe_conf_template.h gui/vlhe_fontscan.h \
             gui/vlhe_mixer.h include/vlhe_status.h include/vsound.h \
             gui/vlhe_mod_volume.h gui/vlhe_mod_cd.h \
             gui/vlhe_mod_midi.h gui/vlhe_mod_status.h \
             gui/vlhe_mod_sound.h gui/vlhe_mod_advanced.h gui/vlhe_icons.h gui/vlhe_self.h \
             gui/vlhe_help.h gui/vlhe_helptext.h VERSION
	@mkdir -p $(PB)
	$(GUICC) -O2 -Wall -I. $(INC) $(GUIDEF) -DVLHE_PORTABLE_BUILD -o $@ \
	    $(CC_SRCS) $(VLHE_RESTART_SRC) $(BACKEND_SRCS) \
	    `$(GTKCFG) --cflags` `$(GTKCFG) --libs`

# sysinfo-vlhe - the report a user runs and sends us when something is
# wrong: this machine, this copy of VLHE, and what the backend SEES.
# It was vlhe-probe until 2026-10-02 - renamed by the user, and moved
# out of $(sbindir) into $(datadir)/tools (tools/ in a portable folder),
# off the PATH, because vlhe-probe beside vlhe, vlhe.gtk and vlhe-setup
# was part of the tab-completion clash (design/31 A6, design/53).
#
# (ORIGINALLY:) what the backend SEES on a real machine.
#
# Five backend pieces are host-tested against files the tests write;
# none had met a real /proc/modules, /dev/mixer or ext2 filesystem.
# This prints what each one finds, so a wrong assumption shows up as an
# odd number rather than as a GUI that draws nothing.
#
# Static, because it runs on a guest that may have nothing staged but
# this file.
# 2026-10-02: also the session and journal code - the report shows the
# session file and DAEMON.LOG's tail, found the way the programs find
# them - and the version from VERSION.
PROBE_SRCS = cli/sysinfo_vlhe.c gui/vlhe_conf.c gui/vlhe_conf_template.c \
             gui/vlhe_fontscan.c gui/vlhe_mixer.c lib/vlhe_status.c gui/vlhe_self.c \
             gui/vlhe_session.c gui/vlhe_journal.c gui/vlhe_modconf.c
PROBE_VER := $(shell cat VERSION 2>/dev/null)

$(B)/sysinfo-vlhe$(X): $(PROBE_SRCS) gui/vlhe_conf.h gui/vlhe_conf_template.h \
                gui/vlhe_fontscan.h gui/vlhe_mixer.h include/vlhe_status.h gui/vlhe_backend.h \
                gui/vlhe_session.h gui/vlhe_journal.h VERSION
	$(TCC) $(STATIC) -O2 -Wall -I. $(INC) $(INSTDEF) -DVLHE_VERSION='"$(PROBE_VER)"' -o $@ $(PROBE_SRCS)

$(PB)/sysinfo-vlhe$(X): $(PROBE_SRCS) gui/vlhe_conf.h gui/vlhe_conf_template.h \
                gui/vlhe_fontscan.h gui/vlhe_mixer.h include/vlhe_status.h gui/vlhe_backend.h \
                gui/vlhe_session.h gui/vlhe_journal.h gui/vlhe_self.h VERSION
	@mkdir -p $(PB)
	$(TCC) $(STATIC) -O2 -Wall -I. $(INC) -DVLHE_PORTABLE_BUILD -DVLHE_VERSION='"$(PROBE_VER)"' -o $@ $(PROBE_SRCS)

# $(B)/vlhe$(X) - the no-X subcommands, linking NO TOOLKIT. That is the
# point: a headless machine, a serial console, or an X server that
# will not start should not need GTK to read a volume level. Static,
# like the daemons, so it works before anything is installed.
# gui/vlhe_apply.c IS NOT IN BACKEND_SRCS - but it is NOT CLI-only: the
# GUI links it too (CC_SRCS above, "gui/vlhe_apply.c IS HERE BECAUSE
# gui/vlhe_cli.c IS"), and its Load buttons run the plan through it.
# Corrected 2026-10-04 (design/54 D42); this said the GUI never applies
# a plan, which stopped being true with the per-row Load buttons.
VLHE_CLI_SRCS = cli/vlhe_cli_main.c gui/vlhe_cli.c gui/vlhe_apply.c gui/vlhe_journal.c \
                $(VLHE_RESTART_SRC) \
                $(BACKEND_SRCS)

$(B)/vlhe$(X): $(VLHE_CLI_SRCS) gui/vlhe_cli.h gui/vlhe_apply.h gui/vlhe_backend.h include/vsound.h
	$(TCC) $(STATIC) -O2 -Wall -I. $(INC) $(INSTDEF) -o $@ $(VLHE_CLI_SRCS)

$(PB)/vlhe$(X): $(VLHE_CLI_SRCS) gui/vlhe_cli.h gui/vlhe_apply.h gui/vlhe_backend.h \
                    include/vsound.h gui/vlhe_self.h
	@mkdir -p $(PB)
	$(TCC) $(STATIC) -O2 -Wall -I. $(INC) -DVLHE_PORTABLE_BUILD -o $@ $(VLHE_CLI_SRCS)

# --- the kernel modules, cross-built ----------------------------------
# THE THIRD BUILD, and it answers what neither of the others can.
#
#   the host build   runs the tests; fast iteration, good diagnostics
#   check-gcc295     will the TARGET's compiler accept this? C89, and
#                    userspace sources only - it compiles against libc
#                    headers and cannot see the kernel's at all
#   module-cross     will this LOAD? Kernel headers, the sound core's
#                    private ones, and the symbol CRCs
#
# The module needs drivers/sound on the include path for sound_config.h
# and dev_table.h, which exist only inside a kernel tree.
#
# Built with the vintage assembler via the corelcc shim: modern gas
# rejects linux/module.h's bare ".section .modinfo" redeclaration.
# KERNELDIR, KCC, KCFLAGS, KPREP, LDR and MODVERSIONS come from defs.mk
# (and host/host.mk here).

# The module sources. Empty until modules/vsound/vsound_dsp.c and modules/vsound/vsound_chan.c exist;
# the target is here first ON PURPOSE, so the first line of module code
# is checked against real kernel headers rather than after the fact.
MODOBJS   = $(B)/obj/modules/vsound/vsound_dsp.o \
            $(B)/obj/modules/vsound/vsound_chan.o \
            $(B)/obj/modules/vsound/vsound_dev.o \
            $(B)/obj/modules/vsound/vsound_drain.o \
            $(B)/obj/modules/vsound/vsound_buf.o \
            $(B)/obj/modules/vsound/vsound_mix.o \
            $(B)/obj/modules/vsound/vsound_conv.o

# The module they link into.
#
# MORE THAN ONE .c PER MODULE, which vcd/ never had - it has one source
# per module and could verify the .o files directly. Here they are
# partial-linked first, because only the linked module carries the
# kernel_version stamp and the complete import list: the stamp comes
# from <linux/module.h>, so only the file including it has one.
MODULE    = $(B)/vsound.o

# The disc module. A SEPARATE module, not more objects in vsound.o, and
# the split is not cosmetic: vsound.o needs the sound core's PRIVATE
# headers (sound_config.h, dev_table.h) and vdisc.o does not, so folding
# them together would make the disc side unbuildable without a kernel
# source tree it has no use for. They also load independently - a
# machine wanting mixing but no virtual CD loads one.
DISCOBJS  = $(B)/obj/modules/vdisc/vdisc_mod.o
DISCMOD   = $(B)/vdisc.o

module: module-cross disc-cross

module-cross:
	@if [ -z "$(MODOBJS)" ]; then \
	    echo "  no module sources yet"; \
	else \
	    if [ ! -f $(KSOUNDDIR)/sound_config.h ]; then \
	        echo "error: $(KSOUNDDIR)/sound_config.h not found."; \
	        echo "       The module needs the sound core's PRIVATE headers,"; \
	        echo "       which exist only in a kernel source tree."; \
	        echo "       INSTALL says how to prepare one."; \
	        exit 1; \
	    fi; \
	    $(KPREP); \
	    for m in $(MODOBJS); do \
	        src=`echo $$m | sed 's|^$(B)/obj/||; s/\.o$$/.c/'`; \
	        printf "  %-18s " "$$src"; \
	        $(KCC) $(KCFLAGS) -I$(KSOUNDDIR) -Iinclude \
	            -c $$src -o $$m || exit 1; \
	        echo "built"; \
	    done; \
	    printf "  %-18s " "$(MODULE)"; \
	    $(LDR) -o $(MODULE) $(MODOBJS) || exit 1; \
	    echo "linked"; \
	fi

# vdisc.o. One source, so no partial link is needed for the symbols -
# but it is still linked through ld -r so the output is a module in the
# same shape as vsound.o, and so adding a second source later does not
# change the rule.
disc-cross:
	@$(KPREP); \
	for m in $(DISCOBJS); do \
	    src=`echo $$m | sed 's|^$(B)/obj/||; s/\.o$$/.c/'`; \
	    printf "  %-18s " "$$src"; \
	    $(KCC) $(KCFLAGS) -Iinclude \
	        -c $$src -o $$m || exit 1; \
	    echo "built"; \
	done; \
	printf "  %-18s " "$(DISCMOD)"; \
	$(LDR) -o $(DISCMOD) $(DISCOBJS) || exit 1; \
	echo "linked"

# --- make install -----------------------------------------------------
# WHERE EACH THING GOES IS WHERE THE CODE ALREADY LOOKS:
#   $(sbindir)  the daemons, `vlhe' (the CLI - vlhe-init looks for it in
#               /usr/sbin and /usr/local/sbin), vlhe.gtk and
#               setup-vlhe with its two files. TOGETHER, because the
#               GUI finds the daemons beside its own binary
#               (daemon_dir_find(), design/49 N8) and setup-vlhe reads
#               its menu and strings from beside itself.
#   $(bindir)   vlhe.gtk as a link for menus and PATH, and smf2wav,
#               which the Render page also looks for in /usr/local/bin
#               and /usr/bin.
#   $(docdir)   LICENSE - the BSD licence asks for its notice to go
#               with binary copies (2026-10-06, the documentation
#               review's finding 4). The package renames it copyright.
#   $(MODDIR)   the modules, then depmod - skipped under DESTDIR, where
#               a package's postinst runs it on the target instead.
#   $(datadir)  vlhe-init, NOT linked into rcS.d - by choice for a source
#               install: enabling boot is the user's act - `make
#               install-boot' (below), which install names. Both
#               questions this once called open are decided (design/41):
#               the package places S60 and K40 with update-rc.d, and its
#               postinst makes our nodes. Also vlhe-account, run here
#               for a real install.
#
# THE SETUID BUILD IS NEVER INSTALLED - it is a portable test build since
# design/55 13f (2026-10-04); `make install-setuid' is gone.
# setup-vlhe WITH THIS BUILD'S VOICE CAP STAMPED IN - it is a shell
# script and cannot ask the compiler, so `make MAX_VOICES=' reaches it
# here. The source keeps the default (512); check-all holds the two
# equal for a default build. Built by `make' (programs) beside vmidid,
# installed by `make install', and staged from here by mkbundle.sh and
# mkxfer.sh - so all three carry the build's cap, not the source's.
#
# .max-voices RECORDS THE VALUE, rewritten only when it changes, so a
# `make MAX_VOICES=1024' after a default build remakes setup-vlhe
# rather than finding it up to date.
$(B)/.max-voices: FORCE
	@mkdir -p $(B)
	@echo $(MAX_VOICES) | cmp -s - $@ 2>/dev/null || echo $(MAX_VOICES) > $@

$(B)/setup-vlhe: scripts/setup-vlhe $(B)/.max-voices
	sed 's/^MAX_Voices=.*/MAX_Voices=$(MAX_VOICES)/' scripts/setup-vlhe > $@
	chmod 755 $@

FORCE:

# WHAT make install PLACES, for the list make uninstall works from - the
# fixed part. The menu entries are added as they are chosen, since where
# they go depends on the machine (below). MANIFEST is that list, written
# at the end of a real install (not under DESTDIR: a package's files are
# dpkg's to remove).
MANIFEST = $(datadir)/installed-files
INSTALLED_FILES = \
	$(sbindir)/vsoundd $(sbindir)/vdiscd $(sbindir)/vmidid $(sbindir)/vlhe \
	$(sbindir)/vlhe.gtk $(sbindir)/setup-vlhe $(sbindir)/setup-vlhe-menu \
	$(sbindir)/setup-vlhe-strings \
	$(bindir)/vlhe $(bindir)/vlhe.gtk $(bindir)/smf2wav \
	$(datadir)/vlhe-init $(datadir)/vlhe-account $(datadir)/vlhe-help.txt \
	$(datadir)/tools/sysinfo-vlhe \
	$(mandir)/man1/vlhe.1 $(mandir)/man1/vlhe.gtk.1 $(mandir)/man1/smf2wav.1 \
	$(mandir)/man1/sysinfo-vlhe.1 $(mandir)/man5/vlhe.conf.5 \
	$(mandir)/man8/setup-vlhe.8 $(mandir)/man8/vsoundd.8 \
	$(mandir)/man8/vdiscd.8 $(mandir)/man8/vmidid.8 \
	$(docdir)/LICENSE \
	$(MODDIR)/vsound.o $(MODDIR)/vdisc.o $(MODDIR)/vmidi.o
# Where the menu entries can be, for an install that left no list (one
# made before 2026-10-06). Only names that are VLHE's own.
MENU_FILES = \
	$(kdedir)/applnk/Applications/Multimedia/vlhe.kdelnk \
	$(kdedir)/applnk/Applications/System/vlhe-root.kdelnk \
	$(kdedir)/applnk/Applications/Multimedia/vlhe-root.kdelnk \
	$(kdedir)/icons/vlhe.xpm $(kdedir)/icons/mini/vlhe.xpm \
	/usr/share/applnk/Multimedia/vlhe.kdelnk \
	/usr/share/applnk/System/vlhe-root.kdelnk \
	/usr/share/applnk/Applications/Multimedia/vlhe.kdelnk \
	/usr/share/applnk/Applications/System/vlhe-root.kdelnk \
	/usr/share/icons/vlhe.xpm /usr/share/icons/mini/vlhe.xpm \
	/usr/share/gnome/apps/Multimedia/vlhe.desktop /usr/share/pixmaps/vlhe.xpm

install: install-guard all $(B)/setup-vlhe
	$(INSTALL) -d $(DESTDIR)$(sbindir) $(DESTDIR)$(bindir) $(DESTDIR)$(datadir)
	@rm -f $(DESTDIR)$(MANIFEST).menus
	$(INSTALL) -m 755 $(B)/vsoundd$(X)   $(DESTDIR)$(sbindir)/vsoundd
	$(INSTALL) -m 755 $(B)/vdiscd$(X)    $(DESTDIR)$(sbindir)/vdiscd
	$(INSTALL) -m 755 $(B)/vmidid $(DESTDIR)$(sbindir)/vmidid
	$(INSTALL) -m 755 $(B)/vlhe$(X)  $(DESTDIR)$(sbindir)/vlhe
	@# AND ON A USER'S PATH, as vlhe.gtk is (below) - 2026-10-05. Only
	@# $(sbindir) had it, which is not on an ordinary user's PATH on
	@# Corel, so `vlhe status' said "command not found" to the user the
	@# help and the man pages tell to run it (found on 86Box). What
	@# needs root still refuses there; the link changes no permission.
	rm -f $(DESTDIR)$(bindir)/vlhe
	ln -s $(sbindir)/vlhe $(DESTDIR)$(bindir)/vlhe
	$(INSTALL) -m 755 $(B)/setup-vlhe $(DESTDIR)$(sbindir)/setup-vlhe
	$(INSTALL) -m 644 scripts/setup-vlhe-menu scripts/setup-vlhe-strings \
	    $(DESTDIR)$(sbindir)
	$(INSTALL) -m 755 $(B)/smf2wav $(DESTDIR)$(bindir)/smf2wav
	$(INSTALL) -m 755 scripts/vlhe-init $(DESTDIR)$(datadir)/vlhe-init
	$(INSTALL) -m 755 scripts/vlhe-account $(DESTDIR)$(datadir)/vlhe-account
	$(INSTALL) -d $(DESTDIR)$(datadir)/tools
	$(INSTALL) -m 755 $(B)/sysinfo-vlhe$(X) $(DESTDIR)$(datadir)/tools/sysinfo-vlhe
	$(INSTALL) -d $(DESTDIR)$(mandir)/man1 $(DESTDIR)$(mandir)/man5 \
	    $(DESTDIR)$(mandir)/man8
	$(INSTALL) -m 644 man/vlhe.1 man/vlhe.gtk.1 man/smf2wav.1 \
	    man/sysinfo-vlhe.1 $(DESTDIR)$(mandir)/man1
	$(INSTALL) -m 644 man/vlhe.conf.5 $(DESTDIR)$(mandir)/man5
	$(INSTALL) -m 644 man/setup-vlhe.8 man/vsoundd.8 man/vdiscd.8 \
	    man/vmidid.8 $(DESTDIR)$(mandir)/man8
	$(INSTALL) -m 644 doc/vlhe-help.txt $(DESTDIR)$(datadir)/vlhe-help.txt
	$(INSTALL) -d $(DESTDIR)$(docdir)
	$(INSTALL) -m 644 LICENSE $(DESTDIR)$(docdir)/LICENSE
	@# THE PRE-2026-10-05 NAMES GO - vlhe-cc (the GUI, and its link in
	@# $(bindir)) and its man page. Left behind by an earlier install they
	@# would sit beside vlhe and vlhe.gtk and bring back the tab-completion
	@# clash the rename removed (design/09, NAMING). A .deb upgrade needs
	@# none of this: dpkg removes the old package's files itself.
	rm -f $(DESTDIR)$(sbindir)/vlhe-cc $(DESTDIR)$(bindir)/vlhe-cc \
	    $(DESTDIR)$(mandir)/man1/vlhe-cc.1
	@if [ "$(BUILD_GUI)" = 1 ]; then \
	    $(INSTALL) -m 755 $(B)/vlhe.gtk$(X) $(DESTDIR)$(sbindir)/vlhe.gtk && \
	    rm -f $(DESTDIR)$(bindir)/vlhe.gtk && \
	    ln -s $(sbindir)/vlhe.gtk $(DESTDIR)$(bindir)/vlhe.gtk && \
	    echo "  vlhe.gtk installed"; \
	fi
	@# THE MENU ENTRIES AND ICONS - design/54 G12. KDE 1's own tree: the
	@# ordinary entry in Applications/Multimedia beside KMix and KsCD,
	@# the root one in Applications/System beside Corel's own "File
	@# Manager (root)" - Corel's place for root tools (2026-10-05, the
	@# user's call; it was beside the ordinary one). Always into a DESTDIR
	@# (the package); on a real install only where this machine has a
	@# KDE menu at all. The old Multimedia copy of the root entry is
	@# removed, for a make install over an earlier one.
	@#
	@# FOUND, NOT ASSUMED, ON A REAL INSTALL - 2026-10-06, Red Hat 6.0,
	@# where nothing was installed and nothing said so. The KDE tree is
	@# $(kdedir)/applnk (Corel) or /usr/share/applnk (Red Hat), and inside
	@# it Applications/Multimedia (Corel) or plain Multimedia (Red Hat:
	@# kdemultimedia's own entries are there) - whichever exists, Corel's
	@# when neither does. GNOME (Red Hat; Corel has none) gets the entry
	@# as a .desktop in /usr/share/gnome/apps/Multimedia. A DESTDIR is the
	@# Corel package, so it keeps Corel's layout. Both entries name
	@# $(bindir) in full: a bare vlhe.gtk is not found where $(bindir)
	@# is off the PATH, which on Red Hat /usr/local/bin is.
	@#
	@# THE ROOT ENTRY ONLY WHERE kvt_kappsu IS - Corel's own way to ask
	@# for the root password (vlhe-root.kdelnk). Red Hat has none, and an
	@# entry that runs a missing program does nothing; it says so instead.
	@if [ "$(BUILD_GUI)" = 1 ]; then \
	    if [ -n "$(DESTDIR)" ]; then rec=/dev/null; else rec=$(MANIFEST).menus; fi; \
	    if [ -n "$(DESTDIR)" ] || [ -d $(kdedir)/applnk ]; then \
	        kde=$(kdedir); \
	    elif [ -d /usr/share/applnk ]; then \
	        kde=/usr/share; \
	    else \
	        kde=""; \
	    fi; \
	    if [ -n "$$kde" ]; then \
	        app=$(DESTDIR)$$kde/applnk; \
	        if [ -n "$(DESTDIR)" ] || [ -d $$app/Applications/Multimedia ] || \
	           [ ! -d $$app/Multimedia ]; then sub=Applications/; else sub=; fi; \
	        $(INSTALL) -d $(DESTDIR)$$kde/icons/mini $$app/$${sub}Multimedia && \
	        $(INSTALL) -m 644 data/vlhe.xpm $(DESTDIR)$$kde/icons/vlhe.xpm && \
	        $(INSTALL) -m 644 data/vlhe-mini.xpm $(DESTDIR)$$kde/icons/mini/vlhe.xpm && \
	        sed 's|@BINDIR@|$(bindir)|' data/vlhe.kdelnk \
	            > $$app/$${sub}Multimedia/vlhe.kdelnk && \
	        chmod 644 $$app/$${sub}Multimedia/vlhe.kdelnk && \
	        rm -f $$app/$${sub}Multimedia/vlhe-root.kdelnk && \
	        echo "  menu: KDE $${sub}Multimedia > VLHE Control Centre" && \
	        echo "        ($$kde/applnk/$${sub}Multimedia/vlhe.kdelnk)" && \
	        { echo $$kde/applnk/$${sub}Multimedia/vlhe.kdelnk; \
	          echo $$kde/icons/vlhe.xpm; echo $$kde/icons/mini/vlhe.xpm; } \
	            >> $$rec || exit 1; \
	        if [ -n "$(DESTDIR)" ] || [ -x /usr/X11R6/bin/kvt_kappsu ] || \
	           [ -x /usr/bin/kvt_kappsu ]; then \
	            $(INSTALL) -d $$app/$${sub}System && \
	            sed 's|@BINDIR@|$(bindir)|' data/vlhe-root.kdelnk \
	                > $$app/$${sub}System/vlhe-root.kdelnk && \
	            chmod 644 $$app/$${sub}System/vlhe-root.kdelnk && \
	            echo "        KDE $${sub}System > VLHE Control Centre (root)" && \
	            echo $$kde/applnk/$${sub}System/vlhe-root.kdelnk >> $$rec || exit 1; \
	        else \
	            echo "        no root entry: kvt_kappsu (Corel's) is not on this"; \
	            echo "        machine - run vlhe.gtk from a root shell instead"; \
	        fi; \
	    else \
	        echo "  menu: no KDE menu found (looked in $(kdedir)/applnk and"; \
	        echo "        /usr/share/applnk) - none installed"; \
	    fi; \
	    if [ -z "$(DESTDIR)" ] && [ -d /usr/share/gnome/apps ]; then \
	        $(INSTALL) -d /usr/share/gnome/apps/Multimedia /usr/share/pixmaps && \
	        $(INSTALL) -m 644 data/vlhe.xpm /usr/share/pixmaps/vlhe.xpm && \
	        sed -e 's|@BINDIR@|$(bindir)|' -e 's|@PIXDIR@|/usr/share/pixmaps|' \
	            data/vlhe.desktop > /usr/share/gnome/apps/Multimedia/vlhe.desktop && \
	        chmod 644 /usr/share/gnome/apps/Multimedia/vlhe.desktop && \
	        echo "  menu: GNOME Multimedia > VLHE Control Centre" && \
	        echo "        (/usr/share/gnome/apps/Multimedia/vlhe.desktop)" && \
	        { echo /usr/share/gnome/apps/Multimedia/vlhe.desktop; \
	          echo /usr/share/pixmaps/vlhe.xpm; } >> $$rec || exit 1; \
	    fi; \
	fi
	@if [ "$(BUILD_MODULES)" = 1 ]; then \
	    $(INSTALL) -d $(DESTDIR)$(MODDIR) && \
	    $(INSTALL) -m 644 $(B)/vsound.o $(B)/vdisc.o $(B)/vmidi.o \
	        $(DESTDIR)$(MODDIR) && \
	    echo "  modules installed in $(MODDIR)"; \
	    if [ -z "$(DESTDIR)" ]; then depmod -a $(MODREL); fi; \
	fi
	@# THE LIST make uninstall REMOVES - every fixed path that is now
	@# there, the menu entries chosen above, and the list itself. Not
	@# under DESTDIR (dpkg removes a package's own files).
	@if [ -z "$(DESTDIR)" ]; then \
	    m=$(MANIFEST); \
	    { for f in $(INSTALLED_FILES); do \
	          if [ -f $$f ] || [ -h $$f ]; then echo $$f; fi; \
	      done; \
	      if [ -f $$m.menus ]; then cat $$m.menus; fi; \
	      echo $$m; } > $$m.new && mv $$m.new $$m && rm -f $$m.menus && \
	    echo "  list of installed files: $$m (make uninstall reads it)" || exit 1; \
	fi
	@# THE DAEMONS' ACCOUNT - design/33 section 3k. Like depmod, run
	@# here only for a real install; a package's postinst runs it.
	@if [ -z "$(DESTDIR)" ]; then sh $(datadir)/vlhe-account; fi
	@# IS $(bindir) ON THE PATH? - 2026-10-06, Red Hat 6.0: /usr/local/bin
	@# is on no PATH there (/etc/profile adds only /usr/X11R6/bin), so
	@# "vlhe: command not found" straight after a clean install. Asked of
	@# the shell that ran make, which is the one the user types into next.
	@if [ -z "$(DESTDIR)" ]; then \
	    case ":$$PATH:" in \
	        *":$(bindir):"*) ;; \
	        *) echo ""; \
	           echo "NOTE: $(bindir) is not on this shell's PATH, so vlhe and"; \
	           echo "vlhe.gtk will not be found by name. For this shell:"; \
	           echo "    PATH=\$$PATH:$(sbindir):$(bindir); export PATH"; \
	           echo "and to keep it, add that line to /etc/profile (everyone) or"; \
	           echo "root's ~/.bash_profile. Or configure with --prefix=/usr." ;; \
	    esac; \
	fi
	@# RED HAT'S pam_console KEEPS THE CARD FROM THE vlhe ACCOUNT - 2026-10-06:
	@# its /etc/security/console.perms gives every sound device to the
	@# console user at 0600, and the pump got "Permission denied" on the
	@# card. The user's call is to widen the rule, not to run the pump as
	@# root; this says how. The same test as vlhe_apply_console_sound_
	@# locked(): on the <console> MODE <sound> MODE2 line, either mode
	@# whose last digit is not 6 or 7 gives others no read and write -
	@# and MODE2 is the one in force at boot, when nobody is logged in.
	@if [ -z "$(DESTDIR)" ] && [ -r /etc/security/console.perms ] && \
	    awk '$$1 == "<console>" && $$3 == "<sound>" && \
	         (substr($$2, length($$2), 1) !~ /[67]/ || \
	          (NF >= 4 && substr($$4, length($$4), 1) !~ /[67]/)) { f = 1 } \
	         END { exit !f }' /etc/security/console.perms; then \
	    echo ""; \
	    echo "NOTE: /etc/security/console.perms keeps the sound devices from"; \
	    echo "the vlhe account the daemons run as. As root, make its <sound>"; \
	    echo "line read"; \
	    echo "    <console> 0666 <sound>     0666 root"; \
	    echo "- both modes: the second is the one in force at boot - then"; \
	    echo "log out and in again."; \
	fi
	@# STARTING AT BOOT, SAID FOR THIS MACHINE'S LAYOUT - 2026-10-06, the
	@# user: "make install should suggest the manual instructions for
	@# making the init script since redhats would be different to corels".
	@# Debian's layout (Corel, potato) and Red Hat's are the two
	@# install-boot knows; anything else gets the script's two calls.
	@echo ""
	@echo "Installed. VLHE does not start at boot yet. As root:"
	@echo "    make install-boot        (make uninstall-boot takes it out)"
	@if [ -z "$(DESTDIR)" ]; then \
	    echo "or by hand:"; \
	    if [ -d /etc/rcS.d ]; then \
	        echo "    cp $(datadir)/vlhe-init /etc/init.d/vlhe"; \
	        echo "    update-rc.d vlhe start 60 S . stop 40 0 1 6 ."; \
	    elif [ -d /etc/rc.d/init.d ]; then \
	        echo "    cp $(datadir)/vlhe-init /etc/rc.d/init.d/vlhe"; \
	        echo '    for n in 2 3 4 5; do ln -s ../init.d/vlhe /etc/rc.d/rc$$n.d/S60vlhe; done'; \
	        echo '    for n in 0 1 6; do ln -s ../init.d/vlhe /etc/rc.d/rc$$n.d/K40vlhe; done'; \
	    else \
	        echo "    this machine has neither /etc/rcS.d nor /etc/rc.d/init.d -"; \
	        echo "    have its boot run '$(datadir)/vlhe-init start' once local"; \
	        echo "    filesystems are mounted and /var/run is cleaned, and"; \
	        echo "    '$(datadir)/vlhe-init stop' at shutdown."; \
	    fi; \
	fi

# uninstall - TAKE OUT WHAT make install PUT IN - 2026-10-06, the user:
# "a make uninstall target that removes our stuff which is proper".
# INSTALL told the user to delete the files by hand, which stopped
# being a fixed list once the menu entries went where each machine
# keeps them.
#
# IT REMOVES WHAT THE LIST NAMES - $(MANIFEST), written by install -
# so it takes nothing it did not put there. Without a list (an install
# before 2026-10-06) it uses the fixed paths and VLHE's own menu file
# names. A line that is not an absolute path, or has `..' in it, is
# skipped rather than trusted.
#
# STOPS A RUNNING VLHE FIRST (vlhe apply -u, since 2026-10-07 - it
# used to refuse and say to run that), so files are not pulled out from
# under running daemons; if the unload fails, nothing is removed.
# REFUSED where the vlhe package is
# installed (dpkg owns those files), without root, and on the
# workstation without DESTDIR (CLAUDE.md section 1). Boot links go
# first, through uninstall-boot. depmod runs after the modules go.
#
# KEPT, AND SAID SO: /etc/vlhe.conf and /etc/vlhe/, everyone's ~/.vlhe,
# /var/lib/vlhe, /var/log/vlhe, /var/run/vlhe, the /dev/vdisc* nodes
# and the vlhe account - settings and the change journal are what a
# reinstall wants back, and the account owns files that may remain.
uninstall:
	@if [ -n "$(HOST_BUILD)" ] && [ -z "$(DESTDIR)" ]; then \
	    echo "make uninstall: refused on the workstation without DESTDIR=."; \
	    echo "  It would remove files from $(prefix) and run depmod against"; \
	    echo "  THIS kernel."; \
	    exit 1; \
	fi
	@if [ -z "$(DESTDIR)" ] && [ "`id -u`" != 0 ]; then \
	    echo "make uninstall: needs root."; exit 1; \
	fi
	@if [ -f $(DESTDIR)/var/lib/dpkg/info/vlhe.list ]; then \
	    echo "make uninstall: the vlhe package is installed - remove it with"; \
	    echo "  dpkg -r vlhe (and its module package), not with make."; \
	    exit 1; \
	fi
	@# STOP A RUNNING VLHE FIRST - the user, 2026-10-07, as the package's
	@# prerm does. "Running" is a module resident OR a daemon whose pid
	@# file names a live process whose PROGRAM is that daemon - argv[0]'s
	@# name, not a word anywhere in its arguments, which an editor open on
	@# vmidid.c would match (a daemon can outlive its module). The unload is the installed vlhe's own; if it fails, or
	@# leaves a module behind, nothing is removed.
	@if [ -z "$(DESTDIR)" ]; then \
	    up=""; \
	    if [ -r /proc/modules ] && \
	        grep -qE '^(vsound|vdisc|vmidi) ' /proc/modules; then \
	        up="a module is loaded"; \
	    fi; \
	    for p in /var/run/vlhe/ctl/vsoundd.pid /var/run/vlhe/ctl/vmidid.pid \
	             /var/run/vlhe/ctl/vdiscd*.pid; do \
	        [ -f "$$p" ] || continue; \
	        pid=`cat "$$p" 2>/dev/null`; \
	        case "$$pid" in ""|*[!0-9]*) continue ;; esac; \
	        n=`basename "$$p" .pid | sed 's/[0-9]*$$//'`; \
	        [ -r /proc/$$pid/cmdline ] || continue; \
	        a0=`tr '\000' '\n' < /proc/$$pid/cmdline | head -1`; \
	        if [ "`basename "$$a0" 2>/dev/null`" = "$$n" ]; then \
	            up="$${up:-$$n is running}"; \
	        fi; \
	    done; \
	    if [ -n "$$up" ]; then \
	        echo "make uninstall: VLHE is running ($$up) - stopping it first."; \
	        if [ ! -x $(sbindir)/vlhe ]; then \
	            echo "make uninstall: $(sbindir)/vlhe is missing, so it cannot"; \
	            echo "  be stopped - nothing was removed."; \
	            exit 1; \
	        fi; \
	        if ! $(sbindir)/vlhe apply -u; then \
	            echo "make uninstall: the unload failed - nothing was removed."; \
	            echo "  Its account is above; fix that, then run make uninstall again."; \
	            exit 1; \
	        fi; \
	        if grep -qE '^(vsound|vdisc|vmidi) ' /proc/modules; then \
	            echo "make uninstall: a VLHE module is still loaded after the"; \
	            echo "  unload - nothing was removed."; \
	            exit 1; \
	        fi; \
	    fi; \
	fi
	@for s in init.d/vlhe rc.d/init.d/vlhe; do \
	    if [ -f $(DESTDIR)/etc/$$s ] && grep -q '^VLHE_STAMP=' $(DESTDIR)/etc/$$s; then \
	        $(MAKE) -s uninstall-boot || exit 1; break; \
	    fi; \
	done
	@m=$(DESTDIR)$(MANIFEST); \
	if [ -f $$m ]; then \
	    echo "removing what $(MANIFEST) lists:"; \
	    list=`cat $$m`; \
	else \
	    echo "no $(MANIFEST) (an older install) - removing VLHE's known files:"; \
	    list="$(INSTALLED_FILES) $(MENU_FILES) $(MANIFEST)"; \
	fi; \
	mods=0; \
	for f in $$list; do \
	    case "$$f" in /*..* | *..*/* ) echo "  skipped (not a plain path): $$f"; continue ;; /*) ;; *) echo "  skipped (not absolute): $$f"; continue ;; esac; \
	    if [ -f $(DESTDIR)$$f ] || [ -h $(DESTDIR)$$f ]; then \
	        rm -f $(DESTDIR)$$f && echo "  $$f"; \
	        case "$$f" in /lib/modules/*) mods=1 ;; esac; \
	    fi; \
	done; \
	for d in $(datadir)/tools $(datadir) $(docdir); do \
	    rmdir $(DESTDIR)$$d 2>/dev/null && echo "  $$d/"; \
	done; \
	if [ $$mods = 1 ] && [ -z "$(DESTDIR)" ]; then depmod -a $(MODREL); fi; \
	true
	@echo ""
	@echo "VLHE is uninstalled. Kept, for a reinstall - remove by hand if"
	@echo "you want them gone:"
	@echo "    /etc/vlhe.conf /etc/vlhe/     the machine's settings"
	@echo "    ~/.vlhe/ (each user's)        their settings and renders"
	@echo "    /var/lib/vlhe /var/log/vlhe   state and the change journal"
	@echo "    /dev/vdisc* /dev/vdiscctl     the drives' device nodes"
	@echo "    the vlhe account              userdel vlhe; groupdel vlhe"

# install-boot / uninstall-boot - STARTING VLHE AT BOOT, FOR A SOURCE
# INSTALL, AS ITS OWN STEP. 2026-10-05, the user's call.
#
# NOT PART OF `make install', on purpose: enabling boot is the user's
# act. (This said the Makefile had no `uninstall', so links `install'
# made would outlive their files; make uninstall exists since
# 2026-10-06 and runs uninstall-boot first.) These two are a pair - what
# install-boot places, uninstall-boot takes out - so the step is
# reversible the way the package's update-rc.d is.
#
# THE LAYOUT IS COREL 1.2's (design/41 section 7): the script copied
# to /etc/init.d/vlhe, S60 in rcS.d, K40 in rc0.d, rc1.d and rc6.d.
#
# AND RED HAT's WHERE THERE IS NO rcS.d - 2026-10-06, found on Red Hat
# 6.0, which has /etc/rc.d/init.d and /etc/rc.d/rc0.d-rc6.d and no
# /etc/init.d at all (initscripts 4.16). The script goes to
# /etc/rc.d/init.d/vlhe, S60 into rc2.d-rc5.d (rcS has no counterpart
# there; rc.sysinit is one script, not a directory), K40 into rc0.d,
# rc1.d and rc6.d. The links read ../init.d/vlhe in both layouts.
# A runlevel-1 boot runs only rc1.d, so a single-user boot skips VLHE
# there without vlhe-init's rcS.d test.
#
# THE RED HAT K40 ONLY RUNS IF /var/lock/subsys/vlhe EXISTS: its rc
# stops a service only when that lock is there, and rc.sysinit empties
# the directory at boot. vlhe-init makes it on start and removes it on
# stop (2026-10-06), wherever /var/lock/subsys exists.
#
# REFUSED where neither layout is present, without root,
# on the workstation without DESTDIR (CLAUDE.md section 1 - it would
# write this machine's /etc), and where the vlhe PACKAGE is installed,
# whose postinst and prerm already own these files.
#
# uninstall-boot REMOVES ONLY WHAT IS OURS: a link only if it points at
# ../init.d/vlhe, and /etc/init.d/vlhe only if it carries vlhe-init's
# VLHE_STAMP line. It does not unload a running VLHE - `vlhe apply -u'
# does that.
BOOT_LINKS = rcS.d/S60vlhe rc0.d/K40vlhe rc1.d/K40vlhe rc6.d/K40vlhe
RH_BOOT_LINKS = rc.d/rc2.d/S60vlhe rc.d/rc3.d/S60vlhe rc.d/rc4.d/S60vlhe \
	rc.d/rc5.d/S60vlhe rc.d/rc0.d/K40vlhe rc.d/rc1.d/K40vlhe \
	rc.d/rc6.d/K40vlhe

boot-guard:
	@if [ -n "$(HOST_BUILD)" ] && [ -z "$(DESTDIR)" ]; then \
	    echo "refused on the workstation without DESTDIR=: it would write"; \
	    echo "  this machine's /etc."; exit 1; \
	fi
	@if [ -z "$(DESTDIR)" ] && [ "`id -u`" != 0 ]; then \
	    echo "needs root - it writes /etc/init.d and /etc/rc?.d."; exit 1; \
	fi
	@if [ ! -d $(DESTDIR)/etc/rcS.d ] && [ ! -d $(DESTDIR)/etc/rc.d/init.d ]; then \
	    echo "no $(DESTDIR)/etc/rcS.d (Corel) or $(DESTDIR)/etc/rc.d/init.d"; \
	    echo "  (Red Hat) - link vlhe-init by hand for this boot layout."; exit 1; \
	fi
	@if [ -f $(DESTDIR)/var/lib/dpkg/info/vlhe.list ]; then \
	    echo "the vlhe package is installed and already starts VLHE at"; \
	    echo "  boot; leave its init script to dpkg."; exit 1; \
	fi

install-boot: boot-guard
	@if [ ! -f $(DESTDIR)$(datadir)/vlhe-init ]; then \
	    echo "no $(DESTDIR)$(datadir)/vlhe-init - run make install first."; \
	    exit 1; \
	fi
	@if [ -d $(DESTDIR)/etc/rcS.d ]; then \
	    initd=init.d; links="$(BOOT_LINKS)"; \
	else \
	    initd=rc.d/init.d; links="$(RH_BOOT_LINKS)"; \
	fi; \
	$(INSTALL) -d $(DESTDIR)/etc/$$initd && \
	$(INSTALL) -m 755 $(DESTDIR)$(datadir)/vlhe-init $(DESTDIR)/etc/$$initd/vlhe && \
	echo "  /etc/$$initd/vlhe" || exit 1; \
	for l in $$links; do \
	    d=`dirname $$l`; \
	    [ -d $(DESTDIR)/etc/$$d ] || mkdir -p $(DESTDIR)/etc/$$d; \
	    rm -f $(DESTDIR)/etc/$$l; \
	    ln -s ../init.d/vlhe $(DESTDIR)/etc/$$l && echo "  /etc/$$l"; \
	done
	@echo "VLHE starts at the next boot. make uninstall-boot takes it out."

uninstall-boot: boot-guard
	@for l in $(BOOT_LINKS) $(RH_BOOT_LINKS); do \
	    f=$(DESTDIR)/etc/$$l; \
	    if [ -h $$f ] && ls -l $$f | grep -q 'init\.d/vlhe$$'; then \
	        rm -f $$f && echo "  removed /etc/$$l"; \
	    fi; \
	done
	@for s in init.d/vlhe rc.d/init.d/vlhe; do \
	    f=$(DESTDIR)/etc/$$s; \
	    if [ -f $$f ]; then \
	        if grep -q '^VLHE_STAMP=' $$f; then \
	            rm -f $$f && echo "  removed /etc/$$s"; \
	        else \
	            echo "  /etc/$$s is not vlhe-init - left alone"; \
	        fi; \
	    fi; \
	done
	@echo "VLHE no longer starts at boot. A running VLHE is still loaded;"
	@echo "vlhe apply -u unloads it."

# deb - VLHE'S DEBIAN PACKAGES, BUILT ON A COREL MACHINE - 2026-10-05.
#
# The user's call: "I want a shipped make deb that users can run on a
# corel machine". scripts/mkdeb packages what `make install' would
# install, as two .debs in DEBDIR:
#
#   vlhe_<VERSION>-1_i386.deb              the programs
#   vlhe-<kernel>-mod_<VERSION>-1_i386.deb the modules, for the kernel
#                                          configure found, with its
#                                          MODVERSIONS setting
#
# So `./configure && make deb' on a stock 1.2 machine makes the stock
# package, and on a 2.2.16-usb one the -usb package; both install side
# by side. Needs root, or fakeroot (the development install has it),
# and dpkg-deb. MAINT names who built it.
#
# REFUSED ON THE WORKSTATION: its dpkg-deb writes members Corel's dpkg
# cannot read. host/staging/mkdeb.sh builds the test packages there.
MAINT  = VLHE local build <root@localhost>
DEBDIR = .

deb: all
	@if [ -n "$(HOST_BUILD)" ]; then \
	    echo "make deb is for a Corel machine - on the workstation use"; \
	    echo "  host/staging/mkdeb.sh, which uses Corel's own dpkg-deb."; \
	    exit 1; \
	fi
	@sh scripts/mkdeb "$(CURDIR)" "$(B)" "$(MODREL)" "$(MODVERSIONS)" \
	    "$(BUILD_GUI)" "$(BUILD_MODULES)" "$(MAINT)" "$(DEBDIR)"

install-guard:
	@if [ -n "$(HOST_BUILD)" ] && [ -z "$(DESTDIR)" ]; then \
	    echo "make install: refused on the workstation without DESTDIR=."; \
	    echo "  It would write $(prefix) and run depmod against THIS kernel"; \
	    echo "  Stage it: make install DESTDIR=/some/dir"; \
	    exit 1; \
	fi

# install-setuid IS GONE - design/55 13f, 2026-10-04. The setuid build is a
# portable test build (`make portable SETUID=1'); an installed setuid GUI
# was design/55 R8's hard-link route to "beside me".

clean:
	rm -rf $(B)
	@$(MAKE) --no-print-directory -C daemons/vmidid clean
	@$(MAKE) --no-print-directory -C modules/vmidi clean

.PHONY: all programs synth modules portable portable-dir install install-guard deb FORCE
.PHONY: uninstall install-boot uninstall-boot boot-guard clean module module-cross disc-cross

# ---- DEVELOPMENT, IN THE GIT TREE ONLY --------------------------------
# The host tests (`make check'), the gates (`make check-all'), the
# workstation's fake GUI and run-* targets, the bench tools and the
# release tarball live in host/dev.mk - split out 2026-10-05, the
# user's call: "all those tests and checks and the gates were used
# during development and a regular user does not need to run them".
# host/ is not in the source tarball (host/mkdist.sh), so there this
# include finds nothing and the Makefile is build, install and portable
# alone. The `-' makes a missing file silent, as defs.mk does for
# host/host.mk.
-include host/dev.mk
