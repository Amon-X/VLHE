# sf2 - our own SoundFont 2.01 container reader.
#
# Copyright (c) 2026 Thomas Tranter
# SPDX-License-Identifier: BSD-3-Clause
#
# Part of VLHE. See LICENSE.TXT for the full license text.
#
# USERSPACE, unlike the rest of vmidi. Built with the target's gcc
# 2.95.2 through tools/corelcc, same as vmidicat, because the synth
# that will use it runs on the target.
#
# `make check' is the part that matters: it parses a real soundfont
# and compares every record against an independent reader. See
# ../../../../vmidi/sf2/tests/compare.sh for why that is the test rather than a unit suite.

VLHE_TOP = ../../..
include $(VLHE_TOP)/defs.mk
CC      = $(TCC)
CFLAGS  = -O2 -Wall -ansi -pedantic
LDFLAGS = -static

# A soundfont to check against. Both of the user's are in the project
# root; the small one is the default because 142 MB is a slow diff.
SF2 = ../../../../Roland.SC-55.sf2

OBJS = sf2.o sf2gen.o sf2mod.o sf2dump.o
BIN  = sf2dump

# The modulator merge has a HOST unit test, like vsound's: the rules
# of 9.5.1 are checked against hand-built cases first, and then
# against the spec test font's own instruments. Built with the host
# compiler so it runs here in a second; the cross-build of sf2mod.o
# above is its C89 check.
HOSTCC = cc
TESTS  = test_sf2mod

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

sf2.o:     sf2.c sf2.h
sf2gen.o:  sf2gen.c sf2gen.h sf2mod.h sf2.h
sf2mod.o:  sf2mod.c sf2mod.h sf2gen.h sf2.h
sf2dump.o: sf2dump.c sf2.h sf2gen.h sf2mod.h

test_sf2mod: ../../host/tests/test_sf2mod.c sf2mod.c sf2gen.c sf2.c sf2mod.h sf2gen.h sf2.h
	$(HOSTCC) -O2 -Wall -I. -o $@ ../../host/tests/test_sf2mod.c sf2mod.c sf2gen.c sf2.c

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

# THE REAL TEST. A parser that compiles proves nothing; one that
# agrees with an independent implementation on 2000+ records of a real
# file proves a great deal.
check: $(BIN) $(TESTS)
	@./test_sf2mod ../../../../vmidi/tests/sfspec/sf_spec_test.sf2
	@sh ../../../../vmidi/sf2/tests/compare.sh "$(SF2)"
	@sh ../../../../vmidi/sf2/tests/zonecheck.sh "$(SF2)"

clean:
	rm -f $(OBJS) $(BIN) $(TESTS)

.PHONY: all check clean
