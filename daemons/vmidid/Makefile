# vmidi/synth - the userspace synth, and its test harness.
#
# Copyright (c) 2026 Thomas Tranter
# SPDX-License-Identifier: BSD-3-Clause
#
# Part of VLHE. See LICENSE for the full license text.
#
# USERSPACE, like vmidi/sf2 and unlike the module. Built with the
# target's gcc 2.95.2 through tools/corelcc.
#
# TWO FRONT ENDS ON ONE RENDERER. render.c knows nothing about where
# its events came from, so the same object serves both:
#
#   smf2wav   a MIDI FILE to a WAV, on the workstation. The
#             development loop - a change can be heard in seconds
#             rather than after staging a bundle and carrying a CF
#             card to the P3. smf.c is its input and is a TEST HARNESS
#             READER, not part of the synth proper.
#   vmidid    /dev/vmidi to /dev/dsp, on the target. THE REAL CHAIN,
#             and the thing the project is for.

# THE TEST TREE DID NOT MOVE with the sources (2026-10-02, design/46):
# it is the old vmidi/synth/tests, reached from here by this path.
VTESTS  = ../../../vmidi/synth/tests

VLHE_TOP = ../..
include $(VLHE_TOP)/defs.mk
# The target compiler - plain gcc natively, host/corelcc here.
CC      = $(TCC)
# OUTPUTS IN vlhe/build/ - binaries there, objects under obj/vmidid
# (2026-10-02, with the top Makefile). `vmidid' and `smf2wav' stay
# valid target names for `make -C daemons/vmidid vmidid smf2wav'.
B       = $(VLHE_TOP)/build
O       = $(B)/obj/vmidid
DUMMY  := $(shell mkdir -p $(O)/sf2)
# -fomit-frame-pointer: render_mix's frame counter and four of its
# per-frame values live on the stack because gcc 2.95.2 has six usable
# registers and %ebx holds the voice (design/20-synth-audit.md 1b).
# Freeing %ebp gives it a seventh: measured 4.794 -> 3.573 s on
# gmstriving before this flag (with #1, #3, #5 of that audit in), and
# bit-identical output. The cost is gdb backtraces of the synth; the
# QEMU/gdb setup on this project was proved out and never needed
# (user, 2026-09-07), and the flag is one edit to drop for a session
# that does.
#
# -D_POSIX_SOURCE BECAUSE -ansi HIDES POSIX. vmidid links
# ../../lib/vlhe_status.c, which calls kill(pid, 0) to ask whether
# another daemon is running - and under -ansi the target's signal.h
# does not declare it, so it compiled with an implicit declaration and
# a warning. The macro is the standard answer and costs nothing else.
#
# WHY IT LINKS A vsound FILE AT ALL: the pidfile code was DUPLICATED
# here, and the duplicate drifted. vsound/vlhe_status.c grew a
# "refuse to start twice" check on 2026-09-23 that this tree's private
# copy never got, so vsoundd and vdiscd refused correctly and vmidid
# still clobbered its own pidfile and exited. The user's precedent:
# "Duplicated code drifting is why 3 scripts turned into 1" - the
# per-machine load.sh wrappers, factored so they could not drift and
# which drifted anyway (CLAUDE.md section 1).
#
# AND -ansi -pedantic IS NOT LOAD-BEARING HERE. It arrived in this
# tree's FIRST COMMIT and was never revisited; vsound/ enforces C89
# the stronger way, by compiling every source with the TARGET's own
# gcc 2.95.2 (its check-gcc295 target), which is a C89 compiler. The
# flag stays because it is harmless and this is not the day to change
# a build that works.
CFLAGS  = -O2 -fomit-frame-pointer -Wall -ansi -pedantic -D_POSIX_SOURCE \
          -Isf2 -I../../include -I../../lib $(VOICEDEF)
LDFLAGS = -static

# -lm FOR THE FILTER. render.c calls sin/cos/sqrt once per note-on to
# compute the biquad coefficients (see render.h on why that stage is
# float). Missing this links fine for smfdump, which does not pull in
# render.o, and fails only on smf2wav - so it was not noticed until a
# script built through the Makefile rather than by hand.
LIBS    = -lm

SF2OBJS = $(O)/sf2/sf2.o $(O)/sf2/sf2gen.o $(O)/sf2/sf2mod.o
OBJS    = $(O)/smf.o $(O)/render.o $(O)/effects.o
BINS    = $(B)/smfdump $(B)/smf2wav $(B)/vmidid $(B)/synthcost

# ------------------------------------------------------------------
# ONE render.o FOR BOTH - the voice array is allocated (2026-10-02)
# ------------------------------------------------------------------
#
# UNTIL 2026-10-02 render.c WAS BUILT TWICE: RENDER_MAX_VOICES sized a
# fixed array in render_state, so smf2wav got a render-offline.o at
# 2048 and the daemon kept 64, because 64 -> 512 cost +511 KB on a
# 16 MB minimum-spec machine.
#
# THE ARRAY NOW FITS THE CEILING ASKED FOR (design/21 section 17), so
# the bound costs nothing and one object serves both. smf2wav defaults
# to 512 and allows 2048; vmidid defaults to 64 and allows 512 - each
# front end states its own cap. smf.c and effects.c were shared all
# along.
OFFOBJS = $(O)/smf.o $(O)/render.o $(O)/effects.o

all: $(BINS)
vmidid: $(B)/vmidid
smf2wav: $(B)/smf2wav
smfdump: $(B)/smfdump
synthcost: $(B)/synthcost
blocktime: $(B)/blocktime
ctlcheck: $(B)/ctlcheck
parsecheck: $(B)/parsecheck
writecheck: $(B)/writecheck

$(B)/smfdump: $(O)/smf.o $(O)/smfdump.o
	$(CC) $(LDFLAGS) -o $@ $(O)/smf.o $(O)/smfdump.o

# smf2wav renders a MIDI file through the synth into a WAV, so a
# change can be HEARD in seconds instead of after staging a bundle and
# carrying a CF card to the P3.
# The same render.o as the daemon since 2026-10-02 - see OFFOBJS.
$(O)/smf2wav.o: smf2wav.c render.h smf.h

$(B)/smf2wav: $(O)/smf2wav.o $(OFFOBJS) $(SF2OBJS)
	$(CC) $(LDFLAGS) -o $@ $(O)/smf2wav.o $(OFFOBJS) $(SF2OBJS) $(LIBS)

# THE DAEMON. Static, like every other non-GUI binary in this project
# - it runs on a target whose libc is the one in usr/.
#
# It does NOT link smf.o: a live byte stream has no file to parse, and
# the parser it does need (running status, messages split across
# reads) is in vmidid.c because it exists only for this input.
# vlhe_status.o IS vsound's, SHARED RATHER THAN COPIED - see CFLAGS.
# It depends on nothing but libc and its own header (366 lines, no
# project includes), so linking it here costs a compile and no
# coupling beyond the one function vmidid needs.
$(O)/vlhe_status.o: ../../lib/vlhe_status.c ../../include/vlhe_status.h
	$(CC) $(CFLAGS) -c -o $@ ../../lib/vlhe_status.c

# The control-channel client lives in lib/ since the move - the GUI
# links the same file.
$(O)/vmidid_ctl.o: ../../lib/vmidid_ctl.c ../../include/vmidid_ctl.h
	$(CC) $(CFLAGS) -c -o $@ ../../lib/vmidid_ctl.c

# autovoice.o - automatic voice reduction, design/21 section 16. The
# daemon's alone: smf2wav does not link it, so an offline render stays
# reproducible (the design's own condition).
$(B)/vmidid: $(O)/vmidid.o $(O)/render.o $(O)/effects.o $(O)/autovoice.o $(O)/vlhe_status.o $(O)/vmidid_ctl.o $(SF2OBJS)
	$(CC) $(LDFLAGS) -o $@ $(O)/vmidid.o $(O)/render.o $(O)/effects.o $(O)/autovoice.o \
	    $(O)/vlhe_status.o $(O)/vmidid_ctl.o $(SF2OBJS) $(LIBS)

$(O)/sf2/%.o: sf2/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(O)/smf.o:     smf.c smf.h
# sf2mod.h is a dependency in its own right: sf2_zone carries an
# sf2_modlist, so a change to that struct changes the zone's size and
# an object built against the old layout reads the second zone of a
# stereo or layered note from the wrong offset. Found 2026-09-08 when
# it did exactly that - garbage envelopes on MT32's two-zone drum
# keys, with sf2gen.o rebuilt and render.o not.
# effects.h is in render.h (reverb_state is inside render_state), so
# it is a dependency of everything render.h is - same reasoning.
$(O)/render.o:  render.c render.h effects.h sf2/sf2.h sf2/sf2gen.h sf2/sf2mod.h
$(O)/effects.o: effects.c effects.h
$(O)/smfdump.o: smfdump.c smf.h
# smf2wav.o's build rule is ABOVE, with the offline define; this is
# only its remaining header dependencies.
$(O)/smf2wav.o: smf.h effects.h sf2/sf2.h sf2/sf2gen.h sf2/sf2mod.h
$(O)/vmidid.o:  vmidid.c render.h effects.h autovoice.h sf2/sf2.h sf2/sf2gen.h sf2/sf2mod.h
$(O)/autovoice.o: autovoice.c autovoice.h

$(O)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Compares the FULL event stream against an independent reader -
# every timestamp, status and data byte. A summary would not catch a
# tempo-change or running-status bug.
#
# synthcost times the renderer block by block ON THE TARGET, through
# the same event loop smf2wav uses (fonts, SysEx, -p, -L, -E, blocks
# split at events), built with THESE flags so the number belongs to
# the daemon that ships - design/20 section 2 found the old blocktime
# figure was an -O3 build's. Per-second lines give cost against
# voices sounding and the filter's share; the summary is blocktime's.
$(B)/synthcost: $(VTESTS)/synthcost.c $(OBJS) $(SF2OBJS) render.h smf.h
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ $(VTESTS)/synthcost.c $(OBJS) $(SF2OBJS) $(LIBS)

# blocktime is KEPT AS IT WAS for the 2026-09-04 Acer record, built
# exactly as its own header says (-O3, its flags), so that number can
# be re-derived. New measurement uses synthcost. Not in BINS: built
# only when asked for, staged if present.
#
# WHICH MEANS A STALE BINARY GETS STAGED. mkbundle.sh stages whatever
# `blocktime' file is here; because this target is not in BINS, `make'
# does not refresh it. On 2026-09-09 the Acer bundle carried a
# blocktime from the previous day and its reading was taken for a
# measurement of that day's three commits - it was not (run 35's
# RUN.txt). BUILD IT EXPLICITLY before staging if its number is
# wanted: `make blocktime'.
$(B)/blocktime: $(VTESTS)/blocktime.c smf.c render.c effects.c
	$(CC) -O3 -mpentium -march=pentium -fomit-frame-pointer \
	    -funroll-all-loops -malign-double -ffast-math $(LDFLAGS) \
	    -I. -Isf2 -I../../include -o $@ $(VTESTS)/blocktime.c smf.c render.c effects.c \
	    sf2/sf2.c sf2/sf2gen.c sf2/sf2mod.c $(LIBS)

# parsecheck covers the OTHER input: vmidid's byte-stream parser,
# whose failures are silent - a running-status mistake plays the wrong
# notes rather than raising an error. It includes vmidid.c as source
# so it tests the shipped parser and not a copy that could drift.
# writecheck covers the block write: vsound returns short after 20 ms
# with a full ring, and until 2026-09-14 the rest of the block was
# thrown away (design/24 B1). Same shape as parsecheck - vmidid.c as
# source, write() stubbed with a scripted driver.
check: $(BINS) $(B)/parsecheck $(B)/writecheck $(B)/ctlcheck check-warnings
	@SMFDUMP=$(B)/smfdump sh $(VTESTS)/smfcheck.sh
	@$(B)/parsecheck
	@$(B)/writecheck
	@$(B)/ctlcheck

# ------------------------------------------------------------------
# check-warnings - A CLEAN BUILD MUST STAY CLEAN
#
# THE USER'S CALL, 2026-09-23: "Keep it flag the warning but we need
# to watch for new warnings."
#
# ONE WARNING IS EXPECTED AND IT IS NOT OURS:
#
#     syslimits.h:7: warning: ANSI C does not allow `#include_next'
#
# GCC 2.95.2's own limits.h reaches the C library's through
# #include_next, which -pedantic objects to. render.c needs limits.h
# for LONG_MAX (filter_out's word-size guard), so the include is
# legitimate and the warning is the toolchain's, not the code's.
#
# IT APPEARED TWICE while render.c was compiled twice (the daemon's
# and render-offline.o); ONCE since the voice array became dynamic,
# 2026-10-02, and one object serves both.
#
# -pedantic STAYS, the user's decision. Dropping it is the only thing
# that silences this (2.95.2 predates -Wno-system-headers, and
# -isystem does not help because the header is found through GCC's
# own path), and a flag is not worth losing over one known line.
#
# SO THE RULE IS MECHANICAL RATHER THAN REMEMBERED. CLAUDE.md: "If a
# rule matters, put it in a script." A note saying "ignore the
# include_next one" is a note that stops being read, and a real
# warning would then sit among the noise - which nearly happened: a
# session dismissed this one as "not our code", and the user's
# correction was that everything compiled here IS ours.
#
# THE CHECK IS A COUNT, not a pattern match on what is new: anything
# other than the known line fails, so a warning nobody predicted
# cannot pass by resembling one that was.
KNOWN_WARNINGS = 1

check-warnings:
	@rm -f $(OBJS) $(O)/render-offline.o $(O)/smfdump.o $(O)/smf2wav.o $(O)/vmidid.o \
	       $(O)/synthcost.o $(O)/vlhe_status.o
	@$(MAKE) --no-print-directory $(BINS) 2>&1 \
	    | grep -i warning > $(B)/.warn.tmp || true
	@n=`grep -c . $(B)/.warn.tmp || true`; \
	 k=`grep -c "include_next" $(B)/.warn.tmp || true`; \
	 if [ "$$n" -eq "$(KNOWN_WARNINGS)" ] && [ "$$k" -eq "$$n" ]; then \
	     echo "  warnings: $$n, all the known syslimits.h one"; \
	 else \
	     echo "  UNEXPECTED WARNINGS - $$n total, $$k known:"; \
	     grep -v "include_next" $(B)/.warn.tmp | sed 's/^/      /'; \
	     rm -f $(B)/.warn.tmp; exit 1; \
	 fi; \
	 rm -f $(B)/.warn.tmp

# THE CONTROL CHANNEL, both ends, with a forked stand-in daemon.
#
# SAFE ON THE WORKSTATION and unlike most things touching vmidid:
# ../../lib/vmidid_ctl.c opens no device, loads no module and starts nothing.
# The test names its own directory through VMIDID_CTL_DIR so a real
# daemon's channel is never touched. See ../../include/vmidid_ctl.h.
$(B)/ctlcheck: ../../host/tests/test_vmidid_ctl.c ../../lib/vmidid_ctl.c ../../include/vmidid_ctl.h
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ ../../host/tests/test_vmidid_ctl.c ../../lib/vmidid_ctl.c

$(B)/parsecheck: $(VTESTS)/parsecheck.c vmidid.c render.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(VTESTS)/parsecheck.c

$(B)/writecheck: $(VTESTS)/writecheck.c vmidid.c render.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(VTESTS)/writecheck.c

clean:
	rm -rf $(O) $(BINS) $(B)/parsecheck $(B)/writecheck $(B)/ctlcheck \
	      $(B)/blocktime $(B)/.warn.tmp

.PHONY: all check check-warnings clean vmidid smf2wav smfdump synthcost blocktime ctlcheck parsecheck writecheck
