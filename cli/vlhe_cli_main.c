/*
 * vlhe_cli_main.c - `main' for the standalone CLI.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * ONE FILE SO THE SUBCOMMANDS CAN LIVE IN TWO BINARIES. vlhe_cli.c
 * holds the work and is linked into the GUI too, so `vlhe volume'
 * behaves identically whether it reaches the code through the GTK
 * binary's dispatch or through this toolkit-free one.
 *
 * C89, GCC 2.95.2.
 */

#include "vlhe_cli.h"

int
main(int argc, char **argv)
{
    return vlhe_cli_main(argc, argv);
}
