/*
 * vlhe_icons.h - the sidebar's icons, as XPM data.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * GTK 1.2 HAS NO IMAGE LOADER AND NO SVG. The one route to a pixmap
 * compiled into the program is `gdk_pixmap_create_from_xpm_d()`, which
 * takes an XPM as an array of strings - so the icons are C source, not
 * files, and the program needs nothing installed beside it.
 *
 * SIXTEEN BY SIXTEEN, and the size is not arbitrary: it is what a
 * GtkCTree row is tall enough for at the stock 1.2 font, and it matches
 * Corel's own Control Center, whose sidebar this deliberately echoes.
 *
 * FIVE COLOURS AND NO MORE. The target runs at 256 colours (verified on
 * the 86Box guest, whose Display Settings reads "256 Colours"), and the
 * desktop has a palette already allocated. An icon reaching for subtle
 * shading either dithers or steals palette entries from the
 * application that needs them. So: black outline, two greys, one
 * accent, and transparency. That is also period-correct - every icon in
 * the reference screenshot is drawn the same way.
 *
 * `None` IS TRANSPARENT and must stay first in each colour table. The
 * mask GDK builds from it is what lets the tree's selection highlight
 * show THROUGH the icon rather than behind a grey box.
 *
 * ASCII only, C89.
 */

#ifndef VLHE_ICONS_H
#define VLHE_ICONS_H

/* A stylised machine - the whole emulated box.
 *
 * IT WAS THE TREE'S ROOT ROW, went unused when the sidebar became a
 * flat list, and IS USED AGAIN by the Status page - which is about the
 * whole machine rather than one part of it, so the icon that once
 * meant "everything below" now means "everything". Kept through the
 * gap rather than deleted, which turned out to be right. */
static char *xpm_vlhe[] = {
"16 16 5 1",
"  c None",
". c #000000",
"+ c #808080",
"@ c #C0C0C0",
"# c #000080",
"                ",
"  ...........   ",
"  .@@@@@@@@@.   ",
"  .@#######@.   ",
"  .@#.....#@.   ",
"  .@#.....#@.   ",
"  .@#.....#@.   ",
"  .@#######@.   ",
"  .@@@@@@@@@.   ",
"  .@@+++++@@.   ",
"  ...........   ",
"    .......     ",
"   .........    ",
"   .+++++++.    ",
"   .........    ",
"                "};

/* Volume. A speaker with sound waves - the one icon whose meaning is
 * universal, and the reference uses the same shape. */
static char *xpm_volume[] = {
"16 16 5 1",
"  c None",
". c #000000",
"+ c #808080",
"@ c #C0C0C0",
"# c #000080",
"                ",
"         .      ",
"       ..#      ",
"     ..@.#   .  ",
"   ..@@@.# .  . ",
" ...@@@@.#.  . .",
" .@@@@@@.#. .. .",
" .@@@@@@.#. .. .",
" .@@@@@@.#. .. .",
" ...@@@@.#.  . .",
"   ..@@@.# .  . ",
"     ..@.#   .  ",
"       ..#      ",
"         .      ",
"                ",
"                "};

/* Sound Settings. A waveform - what the device actually carries, and
 * distinct from the speaker at a glance. */
static char *xpm_sound[] = {
"16 16 5 1",
"  c None",
". c #000000",
"+ c #808080",
"@ c #C0C0C0",
"# c #000080",
"                ",
"  ...........   ",
"  .@@@@@@@@@.   ",
"  .@@@@@@@@@.   ",
"  .@#@@@@@@@.   ",
"  .@#@@#@@@@.   ",
"  .@#@@#@@#@.   ",
"  .###@#@@#@.   ",
"  .@#@@#@@#@.   ",
"  .@#@@#@###.   ",
"  .@@@@#@@#@.   ",
"  .@@@@@@@#@.   ",
"  .@@@@@@@@@.   ",
"  ...........   ",
"                ",
"                "};

/* Midi Settings. A keyboard - five white keys with the black ones
 * between, which reads as MIDI to anyone and as nothing else. */
static char *xpm_midi[] = {
"16 16 5 1",
"  c None",
". c #000000",
"+ c #808080",
"@ c #C0C0C0",
"# c #FFFFFF",
"                ",
"                ",
"  ...........   ",
"  .#########.   ",
"  .#.#.#.#.#.   ",
"  .#.#.#.#.#.   ",
"  .#.#.#.#.#.   ",
"  .#.#.#.#.#.   ",
"  .#.#.#.#.#.   ",
"  .#########.   ",
"  .#.#.#.#.#.   ",
"  .#########.   ",
"  ...........   ",
"                ",
"                ",
"                "};

/* CD Settings. A disc with a centre hole - the reference's own disc
 * icon is the same idea. */
static char *xpm_cd[] = {
"16 16 5 1",
"  c None",
". c #000000",
"+ c #808080",
"@ c #C0C0C0",
"# c #FFFFFF",
"                ",
"     .....      ",
"   ..@@@@@..    ",
"  .@@#####@@.   ",
" .@@#######@@.  ",
" .@####...###@. ",
" .@##..   ..##. ",
" .@##.     .##. ",
" .@##.     .##. ",
" .@##..   ..##. ",
" .@####...###@. ",
" .@@#######@@.  ",
"  .@@#####@@.   ",
"   ..@@@@@..    ",
"     .....      ",
"                "};

#endif /* VLHE_ICONS_H */
