/*
 * cdg.h - CD+G: karaoke graphics carried in the R-W subchannel.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * READ cdtext.h FIRST for the R-W unpack. CD+G and CD-TEXT ride the
 * SAME 96-to-72-byte transform and differ only in what the bytes then
 * mean: CD-TEXT reads them as four 18-byte text packs, CD+G as three
 * 24-byte graphics packets. cdtext_rw_unpack() serves both.
 *
 * THE SCREEN. 300x216 pixels of 6x12 tiles - 50 columns by 18 rows -
 * in 16 colours from a 4096-colour palette. The outer tile all round
 * is border, so a player shows the inner 294x204 and most content
 * sits well inside that.
 *
 * A PACKET, 24 bytes:
 *
 *      0  command      0x09 masked to six bits, else not for us
 *      1  instruction  what to do
 *      2  parityQ      two bytes, unchecked here
 *      4  data         SIXTEEN bytes, each SIX bits significant
 *     20  parityP      four bytes, unchecked
 *
 * SIX BITS PER DATA BYTE, AGAIN. The unpack already stripped the P
 * and Q channels; within a packet every data byte still carries only
 * six significant bits, so a colour index or a pixel row must be
 * masked with 0x3f before use. Missing that gives plausible-looking
 * rubbish rather than an obvious failure.
 *
 * INSTRUCTIONS, and these are MEASURED - every one below appears in
 * the real rip in ExampleCDs/, with counts recorded in
 * tests/fixtures/cdtext/CDG-FINDINGS.md:
 *
 *      1  MEMORY_PRESET     fill the screen with one colour
 *      2  BORDER_PRESET     set the border colour - see the trap below
 *      6  TILE_BLOCK        draw a 6x12 tile, two colours
 *     20  SCROLL_PRESET     scroll, filling with a colour
 *     24  SCROLL_COPY       scroll, wrapping
 *     28  TRANSPARENT       mark one palette entry transparent
 *     30  LOAD_CLUT_LO      palette entries 0-7
 *     31  LOAD_CLUT_HI      palette entries 8-15
 *     38  TILE_BLOCK_XOR    as TILE_BLOCK, but XOR the colours in
 *
 * BORDER_PRESET SETS A COLOUR; IT MUST NOT WRITE PIXELS. THIS IS
 * THE TRAP THAT BREAKS REAL DISCS, and we got it right by accident
 * of method rather than by knowing - recorded 2026-09-26.
 *
 * The obvious reading of "fill the border" is to fill the outer rows
 * and columns of the pixel buffer. **THAT DESTROYS CONTENT A LATER
 * SCROLL WOULD BRING INTO VIEW**, because the 300x216 field is
 * larger than the 288x192 visible area and the margin holds pixels
 * waiting to be scrolled in.
 *
 * VLC DOES EXACTLY THAT AND CANNOT PLAY `Jimi Hendrix - Smash Hits'
 * because of it - Isaac Brodsky's cdgdeck records the finding
 * (`cd-refs/cdgdeck/cdg.cpp:351-373`, four `fillPixels` calls
 * commented out and marked "WRONG - do not use", with "Smash Hits
 * proves that the code above is wrong"). He calls it "a common error
 * without having seen a disc that scrolls graphics into view".
 *
 * `border_preset()' therefore stores the index and nothing else, and
 * `cdg_rgb_at()' resolves it for any coordinate outside the field.
 * That is the same answer cdgdeck reaches, written independently
 * thirteen years apart - **because the instruction table above was
 * MEASURED from the same two discs he used**, Smash Hits and
 * Information Society, both in `ExampleCDs/`.
 *
 * SO A DISC THAT SCROLLS IS THE TEST. A border implemented by
 * filling looks correct on every disc that does not.
 *
 * AN UNKNOWN INSTRUCTION IS IGNORED, NOT AN ERROR. The real rip
 * carries three singleton instructions (22, 15, 7) that are almost
 * certainly read errors off the disc, and a decoder that stopped at
 * the first one would render nothing for a disc that plays fine.
 *
 * REFERENCES READ AND NOT COPIED, as for every reference tree here
 * (CLAUDE.md section 5) - we ship BSD-3:
 *
 *   vcd/contrib/refs/cdemu/   the R-W unpack on the writing side,
 *                             device-recording.c:180-189
 *   ExampleCDs/               two real karaoke discs, one 2448
 *                             interleaved and one with a separate
 *                             .cdg, which is what made the
 *                             instruction table above measurable
 *
 * C89 / GCC 2.95 clean, and NO FLOATING POINT - this runs on a
 * Pentium II under an emulator, and design/07 records what that
 * costs elsewhere.
 */

#ifndef _VDISC_CDG_H
#define _VDISC_CDG_H

/* A packet on the wire, and the three that come out of one sector. */
#define CDG_PACKET_SIZE     24
#define CDG_PACKETS_PER_SEC 3
#define CDG_DATA_BYTES      16

/* The command byte, masked to its six significant bits. */
#define CDG_COMMAND         0x09

/* Instructions. */
#define CDG_MEMORY_PRESET   1
#define CDG_BORDER_PRESET   2
#define CDG_TILE_BLOCK      6
#define CDG_SCROLL_PRESET   20
#define CDG_SCROLL_COPY     24
#define CDG_TRANSPARENT     28
#define CDG_LOAD_CLUT_LO    30
#define CDG_LOAD_CLUT_HI    31
#define CDG_TILE_BLOCK_XOR  38

/* The screen, in pixels and in tiles. */
#define CDG_WIDTH           300
#define CDG_HEIGHT          216
#define CDG_TILE_W          6
#define CDG_TILE_H          12
#define CDG_COLS            (CDG_WIDTH / CDG_TILE_W)    /* 50 */
#define CDG_ROWS            (CDG_HEIGHT / CDG_TILE_H)   /* 18 */
#define CDG_COLOURS         16

/*
 * THE DECODED SCREEN.
 *
 * One byte per pixel, holding a PALETTE INDEX rather than a colour -
 * a LOAD_CLUT arriving later changes what is already drawn, which is
 * how these discs do fades, and storing resolved colours would freeze
 * them.
 *
 * `clut' entries are 0x0RGB, four bits each, as the disc carries
 * them. A caller scales to its own depth.
 */
/*
 * WHICH TILES CHANGED, so a caller can repaint only those.
 *
 * ONE BIT PER TILE, 50 x 18. A bounding box was tried first and is
 * not enough: measured on the Information Society disc, three polls
 * in the first five seconds have a box covering 55-74% of the screen
 * while only ~45 tiles (5%) actually changed. That disc draws a
 * banner RIGHT TO LEFT across the top while the lyrics run LEFT TO
 * RIGHT along the bottom, so a box grows from both ends at once and
 * a poll's real work is two small regions at opposite corners.
 *
 * A caller walks the rows and sends one span per row, from its
 * leftmost changed tile to its rightmost. A row nothing touched
 * costs nothing, so the cost tracks the number of ACTIVE rows rather
 * than the distance between them - which is the property the box
 * could not have.
 */
#define CDG_TILE_WORDS  ((CDG_COLS + 31) / 32)      /* 2 per row */

struct cdg_screen {
    unsigned char  pixel[CDG_WIDTH * CDG_HEIGHT];
    unsigned short clut[CDG_COLOURS];
    int            border;          /* palette index of the border   */
    int            transparent;     /* index, or -1                  */
    int            h_offset;        /* 0-5,  set by scroll           */
    int            v_offset;        /* 0-11, set by scroll           */
    int            dirty;           /* something changed since reset */

    /*
     * `touched' IS NOT `dirty', AND THE DIFFERENCE MATTERS.
     *
     * `dirty' means "a decoder instruction has painted something
     * since the reset" and is the older flag; four host tests assert
     * its exact behaviour (test_cdg.c:59,165,174,255 and
     * test_image_sub.c:109), so nothing here may change it.
     *
     * `touched' means "these tiles have not been pushed to the
     * display yet", and the DISPLAY clears it - cdg_take_tiles().
     * The two differ at exactly one moment: cdg_reset() has painted
     * nothing (dirty stays 0) but the cleared screen does have to be
     * drawn once (touched is set for every tile).
     */
    unsigned long  touched[CDG_ROWS * CDG_TILE_WORDS];
};

/*
 * MARK TILES CHANGED. `cdg_mark_tiles' takes a half-open tile range;
 * `cdg_mark_all' is the whole screen, for anything that rewrites it
 * (a preset, a palette change, a scroll).
 *
 * BOTH SET `dirty' TOO, so the decoder keeps its old behaviour.
 */
void cdg_mark_tiles(struct cdg_screen *s, int row0, int col0,
                    int row1, int col1);
void cdg_mark_all(struct cdg_screen *s);

/* Is tile (row, col) waiting to be drawn? */
int cdg_tile_touched(const struct cdg_screen *s, int row, int col);

/*
 * THE CHANGED SPAN OF ONE TILE ROW, and clearing it.
 *
 * Returns 1 and fills in col0 and col1 (half-open, in tiles) when
 * that row has anything waiting, 0 when it has not. `cdg_take_tiles'
 * then
 * clears the whole map - a caller draws every row first, then calls
 * it once.
 */
int  cdg_row_span(const struct cdg_screen *s, int row,
                  int *col0, int *col1);
void cdg_take_tiles(struct cdg_screen *s);

/* Start a screen: black, empty, no transparency. */
void cdg_reset(struct cdg_screen *s);

/*
 * APPLY ONE 24-BYTE PACKET. Returns 1 if it changed the screen, 0 if
 * it was not ours or was an instruction we ignore.
 */
int cdg_packet(struct cdg_screen *s, const unsigned char *pkt);

/*
 * APPLY ONE SECTOR'S WORTH OF SUBCHANNEL - AND THERE IS MORE THAN
 * ONE LAYOUT IN THE WILD. Measured 2026-09-20 across the two karaoke
 * discs in ExampleCDs/, which disagree:
 *
 *   CDG_SUB_PW96_INTERLEAVED  96 bytes of raw P-W as the disc carries
 *                         them, six significant bits per byte.
 *                         Unpacks to 72 = THREE packets.
 *                         (Information Society's `.sub')
 *
 *   CDG_SUB_RW96          96 bytes of already-deinterleaved R-W,
 *                         "cooked" - FOUR 24-byte CD+G packets, no
 *                         unpack needed.
 *                         (the Jimi Hendrix 2448 `.raw')
 *
 * THE NAMES ARE cdemu's, DELIBERATELY. libMirage enumerates four
 * formats where this file first had two
 * (`libmirage/mirage/fragment.h:55-61`): PW96_INTERLEAVED, PW96_LINEAR
 * - all 96 bytes present but channel by channel rather than
 * interleaved - RW96, and Q16, with INTERNAL/EXTERNAL as a separate
 * axis for whether the data sits in the track file or beside it.
 * Their vocabulary is better than the one invented here and
 * PW96_LINEAR is a real case we would otherwise meet and misread.
 * **We implement two of the four**; a linear or Q16 rip is not
 * handled and would need its own transform.
 *
 * THE DIFFERENCE IS NOT SUBTLE AND IT IS NOT GUESSWORK. Counting
 * command-0x09 packets over the first 20,000 sectors of each disc,
 * read DIRECTLY as four packets:
 *
 *   Jimi Hendrix 2448 .raw   55,592 of 80,000   69.5%
 *   Information Society .sub      11 of 80,000    0.0%
 *
 * and under the unpack the Information Society disc gives 425 where
 * the direct reading gives 11. Each disc is unambiguous; they are
 * simply not the same format.
 *
 * SO A CALLER MUST SAY WHICH, and cdg_sniff() will say which a given
 * buffer looks like when the container does not record it.
 *
 * AND THE CONTAINER GENUINELY DOES NOT SAY - cdemu PROVES IT, by
 * getting this wrong. `image-cue/parser.c:410-413' sets every
 * 2448-byte CUE track to PW96_INTERLEAVED and carries the comment
 *
 *     FIXME: what format of subchannel is there anyway?
 *
 * That is the actively maintained reference implementation admitting
 * a CUE does not record the format - and the assumption is WRONG for
 * the Jimi Hendrix disc, which is a 2448 CUE holding cooked RW96. On
 * that disc this code path renders nothing.
 *
 * So sniffing is not a shortcut around reading the container
 * properly; it is the only thing that works, and the margins below
 * make it safe.
 *
 * Returns how many packets changed the screen.
 */
#define CDG_SUB_PW96_INTERLEAVED 0
#define CDG_SUB_RW96             1

int cdg_sector(struct cdg_screen *s, const unsigned char *sub96,
               int layout);

/*
 * WHICH LAYOUT IS THIS? Tries both over `n' sectors and returns the
 * one that yields more valid CD+G packets, or
 * CDG_SUB_PW96_INTERLEAVED when neither does - which is also the right answer for a disc with
 * no graphics at all, since nothing will decode either way.
 *
 * A HEURISTIC, AND NAMED AS ONE. Prefer knowing from the container:
 * a `.sub' beside a CloneCD set is interleaved, a 2448-byte rip is
 * what its own ripper chose. Use this when nothing says.
 */
int cdg_sniff(const unsigned char *sub, int n_sectors);

/*
 * RESOLVE ONE PIXEL TO 0x0RGB, following the palette as it stands.
 * Out-of-range coordinates give the border colour, which is what a
 * player draws there anyway.
 */
unsigned short cdg_rgb_at(const struct cdg_screen *s, int x, int y);

#endif /* _VDISC_CDG_H */
