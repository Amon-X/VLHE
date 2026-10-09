/*
 * vlhe_conf_template.h - the keys, their defaults, and their comments,
 * as DATA.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * WHY THIS IS A TABLE AND NOT A SEQUENCE OF fprintf CALLS. The
 * comments are NOT SETTLED (the user, 2026-09-18: "I would want the
 * comments to easily be changed as I have not decided on what the
 * comments should say"). Scattered through C as string literals,
 * changing one would mean finding it among the writer's logic and
 * rebuilding. Here it is one line of data.
 *
 * IT ALSO KEEPS A PROMISE THE FILE MAKES ABOUT ITSELF. The example
 * says "every value below is the CURRENT DEFAULT unless the comment
 * says otherwise, so a file of nothing but section headers behaves as
 * the software does today". That only holds if the defaults and the
 * comments live together, which they do here - they cannot drift
 * apart into two lists that disagree.
 *
 * THE SOURCE IS `design/vsound.conf.example`, and the text below is
 * taken from it. When that file changes, this table changes; they are
 * the same document in two forms, and the example says so in its own
 * header.
 *
 * WHAT IS DELIBERATELY ABSENT, and the example explains each: the
 * tuning knobs that exist for diagnosis (`vsound_depth`,
 * `vsound_ratelimit`, `vsound_write_ms`), `vmidi_minor` (fixed by the
 * device node), the test-harness options, and `Channels` - which is a
 * COMPILE-TIME constant, so a key here would be read by nothing.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_CONF_TEMPLATE_H
#define VLHE_CONF_TEMPLATE_H

#include "vlhe_conf.h"

/* WHICH FILE A KEY BELONGS TO - design/33 section 1. The three do not
 * overlap except for the mixer levels, which are a boot-time floor in
 * the system file overridden by the user's own at login. */
#define VLHE_TPL_SYSTEM  0      /* /etc/vlhe.conf       */
#define VLHE_TPL_USER    1      /* ~/.vlhe/vlhe.conf    */
#define VLHE_TPL_DRIVES  2      /* /var/lib/vlhe/state/drives */

struct vlhe_tpl_entry {
    int         file;           /* VLHE_TPL_*                          */
    const char *section;        /* "Sound Settings"                    */
    const char *key;            /* "MidiChannel"                       */
    const char *dflt;           /* the CURRENT default, as text        */

    /* The explanatory block written ABOVE the key, each line already
     * carrying its own "; ". NULL for none. A trailing newline is
     * supplied by the writer, so these do not end in one. */
    const char *comment;

    /* WRITTEN COMMENTED OUT. For a key that documents something the
     * software does but which nothing reads - `Channels' is the case
     * the example carries. The user sees why it is there and that
     * editing it would do nothing. */
    int         disabled;
};

/* The table, and its length. */
extern const struct vlhe_tpl_entry vlhe_conf_template[];
extern const int vlhe_conf_template_n;

/* The header block for each file, written above everything. */
const char *vlhe_conf_template_header(int file);

/* Fill `c' with every key belonging to `file', at its default. The
 * result written out is a complete, self-documenting file that
 * behaves exactly as the software does with no config at all. */
int vlhe_conf_template_defaults(struct vlhe_conf *c, int file);

/* Write `c' to `path' in template order, with each key's comment above
 * it - the whole point of this file.
 *
 * KEYS IN `c' THAT THE TEMPLATE DOES NOT KNOW ARE STILL WRITTEN, after
 * the known ones in their section. A key we stopped recognising must
 * not be silently dropped from a user's file.
 *
 * `backup' and the write sequence are vlhe_conf_write()'s. */
int vlhe_conf_template_write(const struct vlhe_conf *c, const char *path,
                             int file, int backup);

#endif /* VLHE_CONF_TEMPLATE_H */
