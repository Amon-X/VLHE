/*
 * vlhe_match.c - the filename glob the "Files of type" dropdown uses.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * SPLIT OUT OF vlhe_filter.c 2026-09-22 so it can be HOST-TESTED.
 * That file includes <gtk/gtk.h>, which the workstation's gcc does
 * not have; this one includes nothing but <string.h>, so
 * test_vlhe_match.c can include it directly the way every other test
 * here includes its subject.
 *
 * IT WAS SPLIT BECAUSE A BUG GOT THROUGH. A session wrote `*.[ci]*'
 * for the CD page's three image types, on the assumption that a
 * character class would work. There are none - a `[' is matched
 * literally - so the pattern matched NOTHING, and the symptom would
 * have been an empty file list rather than an error. A scratch
 * program caught it; nothing in `make check-all' could have.
 *
 * CLAUDE.md: "If a rule matters, put it in a script." A matcher two
 * pages depend on, whose failure mode is showing no files, is exactly
 * that.
 *
 * C89, GCC 2.95.2.
 */

#include <string.h>

#include "vlhe_match.h"

/*
 * DOES THIS NAME MATCH ONE PATTERN, given as [pat, end)?
 *
 * fnmatch-style, and hand-written because the target's libc has
 * fnmatch but design/32 measured the patterns we actually need - one
 * extension, `*.mid*' - and a full fnmatch is more surface than that
 * requires.
 *
 * Handles `*' and `?' and literal text. Case-insensitive, because a
 * file called SONG.MID is a MIDI file and an ISO-9660 disc hands
 * back upper case.
 *
 * THERE IS NO `[abc]' CHARACTER CLASS, and that is worth stating
 * because a `[' in a pattern is matched LITERALLY rather than
 * refused - so `*.[ci]*' silently matches nothing at all. A session
 * wrote exactly that on 2026-09-22 and the host test above caught it
 * before it shipped; `;' below is the alternation to use instead.
 */
static int
match_one(const char *name, const char *pat, const char *end)
{
    while (pat < end) {
        if (*pat == '*') {
            pat++;
            if (pat == end)
                return 1;               /* trailing * matches all */
            while (*name != '\0') {
                if (match_one(name, pat, end))
                    return 1;
                name++;
            }
            return 0;
        }
        if (*name == '\0')
            return 0;
        if (*pat != '?') {
            int a = *name, b = *pat;

            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b)
                return 0;
        }
        name++;
        pat++;
    }
    return *name == '\0';
}

/*
 * SEVERAL PATTERNS, SEPARATED BY `;' - any one matching is a match.
 *
 * ADDED 2026-09-22 for the CD page, whose three backends are .cue,
 * .ccd and .iso: "Disc images" has to offer all three at once, and
 * there is no way to say that in one glob without character classes
 * this matcher does not have.
 *
 * `;' RATHER THAN `,' because a comma is legal in a filename and
 * appears in the LABELS already ("*.mid, *.midi"), where a semicolon
 * would read as punctuation. It is also what Windows file dialogs
 * have used for this since 3.1, so it is the spelling anyone who has
 * typed one before will expect.
 *
 * A pattern with no `;' behaves exactly as it did, which is what
 * keeps every existing filter working unchanged.
 */
int
name_matches(const char *name, const char *pat)
{
    const char *semi;

    while ((semi = strchr(pat, ';')) != NULL) {
        if (match_one(name, pat, semi))
            return 1;
        pat = semi + 1;
    }
    return match_one(name, pat, pat + strlen(pat));
}
