/*
 * vlhe_cli.h - the no-X subcommands.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * `vlhe volume' and, as they are built, `vlhe apply', `vlhe status'
 * and `vlhe setup'. design/33 section 1d has why these are
 * subcommands rather than flags.
 *
 * SEPARATE FROM THE GUI SO IT LINKS NO TOOLKIT: a headless machine or
 * a broken X server should not need GTK to read a volume level.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_CLI_H
#define VLHE_CLI_H

#include <stdio.h>

/* Does this argument start a subcommand? A bare word does; anything
 * beginning with `-' is a GUI flag. One rule, no collisions. */
int  vlhe_cli_is_subcommand(const char *arg);

/* Run the subcommand named by argv[1]. Returns a process exit code. */
int  vlhe_cli_main(int argc, char **argv);

void vlhe_cli_usage(FILE *fp);

#endif /* VLHE_CLI_H */
