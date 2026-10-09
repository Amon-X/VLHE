# vmidi - a MIDI device that a userspace synth reads.
#
# Copyright (c) 2026 Thomas Tranter
# SPDX-License-Identifier: BSD-3-Clause
#
# Part of VLHE. See LICENSE for the full license text.
#
# design/p3-midi-plan.md has the design. This builds ONE module,
# vmidi.o, cross-compiled with the target's own GCC 2.95.2 exactly as
# vsound's is - see vsound/Makefile, from which the flags below are
# taken unchanged.
#
# WHY ITS OWN DIRECTORY AND ITS OWN MODULE. vmidi is optional: a
# machine that wants mixing but no MIDI loads vsound and not this. The
# only coupling is vsound's reserved channel (vsound_midi=1), and even
# that is a module parameter rather than a build dependency.

VLHE_TOP = ../..
include $(VLHE_TOP)/defs.mk
# KERNELDIR, KCC, KCFLAGS, KPREP, LDR, MODVERSIONS: defs.mk.

# OUTPUTS IN vlhe/build/ (2026-10-02): the module there, its object
# under obj/modules/vmidi.
B       = $(VLHE_TOP)/build
DUMMY  := $(shell mkdir -p $(B)/obj/modules/vmidi)
MODOBJS = $(B)/obj/modules/vmidi/vmidi_mod.o
MODULE  = $(B)/vmidi.o

# The userspace side. STATIC, like vsoundd and vdiscd - it has to run
# on a target whose libc is the one in usr/, and static linking is what
# the rest of this project uses for anything without a GUI.
TOOLS   = $(B)/vmidicat

all: module tools

tools: $(TOOLS)
vmidicat: $(B)/vmidicat

$(B)/vmidicat: ../../host/vmidicat.c
	@printf "  %-18s " "$<"
	@$(TCC) -O2 -Wall -Wstrict-prototypes -o $@ $<
	@echo "built"


module:
	@if [ ! -f $(KSOUNDDIR)/sound_config.h ]; then \
	    echo "error: $(KSOUNDDIR)/sound_config.h not found."; \
	    echo "       vmidi needs the sound core's PRIVATE headers"; \
	    echo "       (sound_config.h, dev_table.h), which exist only"; \
	    echo "       in a kernel source tree. INSTALL says how to prepare one."; \
	    exit 1; \
	fi
	@$(KPREP)
	@for m in $(MODOBJS); do \
	    src=`basename $$m .o`.c; \
	    printf "  %-18s " "$$src"; \
	    $(KCC) $(KCFLAGS) $(EXTRA_KCFLAGS) -I$(KSOUNDDIR) \
	        -c $$src -o $$m || exit 1; \
	    echo "built"; \
	done
	@$(LDR) -o $(MODULE) $(MODOBJS)
	@echo "  $(MODULE)           linked"

# EVERY IMPORTED SYMBOL CHECKED against the target kernel's .ver files.
#
# The same recipe as vsound/Makefile's verify-one, and it matters for
# the same reason: MODVERSIONS means a module built against the wrong
# kernel fails at insmod with a CRC mismatch that reads like
# corruption. This catches it here instead.
verify-module: module
	@stamp=`strings -a $(MODULE) | grep '^kernel_version=' || true`; \
	if [ -z "$$stamp" ]; then \
	    echo "  $(MODULE): kernel stamp MISSING"; exit 1; \
	fi; \
	echo "  $(MODULE): $$stamp"; \
	fail=0; \
	for s in `nm $(MODULE) | sed -n 's/.* U \([a-z_][a-z_0-9]*\)_R[0-9a-f]*$$/\1/p'`; do \
	    m=`nm $(MODULE) | sed -n "s/.* U $${s}_R\([0-9a-f]*\)$$/\1/p" | head -1`; \
	    k=`grep -h "__ver_$${s}\b" $(KERNELDIR)/include/linux/modules/*.ver \
	         2>/dev/null | awk '{print $$3}' | head -1`; \
	    if [ -z "$$k" ]; then \
	        echo "    $$s: NOT EXPORTED by this kernel"; fail=1; \
	    elif [ "$$m" != "$$k" ]; then \
	        echo "    $$s: CRC $$m != kernel $$k"; fail=1; \
	    fi; \
	done; \
	[ $$fail -eq 0 ] && echo "    all imported symbols match the kernel" || exit 1

clean:
	@rm -f $(MODOBJS) $(MODULE) $(TOOLS)

.PHONY: all module tools vmidicat verify-module clean
