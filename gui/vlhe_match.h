/*
 * vlhe_match.h - the "Files of type" glob.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * See vlhe_match.c for why this is its own file: it is host-tested,
 * and vlhe_filter.c cannot be because it includes GTK.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_MATCH_H
#define VLHE_MATCH_H

/*
 * DOES `name' MATCH `pat'?
 *
 *   *    any run of characters, including none
 *   ?    exactly one character
 *   ;    separates ALTERNATIVES - any one matching is a match
 *
 * Case-insensitive: a file called SONG.MID is a MIDI file, and an
 * ISO-9660 disc hands names back in upper case.
 *
 * THERE ARE NO CHARACTER CLASSES. `[abc]' is matched LITERALLY, so a
 * pattern written that way matches nothing and fails silently - use
 * `;' instead. This is the mistake the file was split out to catch.
 *
 * Returns non-zero on a match.
 */
int name_matches(const char *name, const char *pat);

#endif /* VLHE_MATCH_H */
