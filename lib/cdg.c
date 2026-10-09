/*
 * cdg.c - CD+G graphics packets. READ cdg.h FIRST; it has the packet
 * layout, the instruction table (measured, not assumed) and why an
 * unknown instruction is ignored rather than fatal.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * C89, GCC 2.95.2, no floating point.
 */

#include <string.h>

#include "cdg.h"
#include "cdtext.h"     /* cdtext_rw_unpack - the shared transform */
#include "vdisc.h"      /* VDISC_SUB_SIZE */

/* Every data byte carries six significant bits. */
#define SIX(x)  ((unsigned char)((x) & 0x3f))

/* ------------------------------------------------------------------ */

void
cdg_reset(struct cdg_screen *s)
{
    if (s == NULL)
        return;

    memset(s, 0, sizeof *s);
    s->transparent = -1;        /* 0 is a real colour index */

    /*
     * THE CLEARED SCREEN STILL HAS TO BE DRAWN ONCE, and this is the
     * one place `touched' and `dirty' must disagree.
     *
     * The memset above wipes the tile map, so without this a reset
     * would leave the display showing the PREVIOUS disc until the
     * next tile happened to arrive. `vlhe_mod_cdg.c' calls this from
     * eight places - stop, disc change, track change, detach - and
     * requiring each to remember a mark afterwards is the kind of
     * rule that gets forgotten at the ninth call site. It lives here
     * instead, where it cannot be.
     *
     * `dirty' STAYS 0: nothing has been PAINTED by a decoder
     * instruction, and test_cdg.c:59 asserts exactly that after a
     * reset. So the bits are set directly rather than through
     * cdg_mark_all(), which would set `dirty' too.
     */
    memset(s->touched, 0xff, sizeof s->touched);
}

/* ------------------------------------------------------------------ */
/* Which tiles are waiting to be drawn                                */
/* ------------------------------------------------------------------ */

void
cdg_mark_tiles(struct cdg_screen *s, int row0, int col0,
               int row1, int col1)
{
    int r;

    if (s == NULL)
        return;

    /*
     * CLAMP RATHER THAN TRUST. row and col reach here from bytes off
     * the disc, and a scratched sector carries anything - the same
     * reason tile_block() bounds-checks before it indexes.
     */
    if (row0 < 0) row0 = 0;
    if (col0 < 0) col0 = 0;
    if (row1 > CDG_ROWS) row1 = CDG_ROWS;
    if (col1 > CDG_COLS) col1 = CDG_COLS;

    s->dirty = 1;               /* the OLD flag, unchanged behaviour */

    for (r = row0; r < row1; r++) {
        int c;

        for (c = col0; c < col1; c++)
            s->touched[r * CDG_TILE_WORDS + (c >> 5)] |=
                (unsigned long) 1 << (c & 31);
    }
}

void
cdg_mark_all(struct cdg_screen *s)
{
    cdg_mark_tiles(s, 0, 0, CDG_ROWS, CDG_COLS);
}

int
cdg_tile_touched(const struct cdg_screen *s, int row, int col)
{
    if (s == NULL || row < 0 || row >= CDG_ROWS ||
        col < 0 || col >= CDG_COLS)
        return 0;
    return (s->touched[row * CDG_TILE_WORDS + (col >> 5)] &
            ((unsigned long) 1 << (col & 31))) ? 1 : 0;
}

int
cdg_row_span(const struct cdg_screen *s, int row, int *col0, int *col1)
{
    int c, lo = -1, hi = -1;

    if (s == NULL || row < 0 || row >= CDG_ROWS)
        return 0;

    for (c = 0; c < CDG_COLS; c++) {
        if (!cdg_tile_touched(s, row, c))
            continue;
        if (lo < 0)
            lo = c;
        hi = c;
    }

    if (lo < 0)
        return 0;               /* nothing waiting in this row */

    if (col0 != NULL) *col0 = lo;
    if (col1 != NULL) *col1 = hi + 1;
    return 1;
}

void
cdg_take_tiles(struct cdg_screen *s)
{
    if (s == NULL)
        return;
    memset(s->touched, 0, sizeof s->touched);
}

/* ------------------------------------------------------------------ */
/* Instructions                                                       */
/* ------------------------------------------------------------------ */

static void
memory_preset(struct cdg_screen *s, const unsigned char *d)
{
    unsigned char colour = SIX(d[0]) & 0x0f;

    /*
     * THE REPEAT FIELD IS IGNORED, DELIBERATELY. d[1] holds a repeat
     * count because the same preset is sent up to sixteen times in
     * case of a read error; acting on each is idempotent, so honouring
     * the count would only re-fill an already-filled screen.
     */
    memset(s->pixel, colour, sizeof s->pixel);
    cdg_mark_all(s);            /* every pixel rewritten */
}

static void
border_preset(struct cdg_screen *s, const unsigned char *d)
{
    s->border = SIX(d[0]) & 0x0f;
    cdg_mark_all(s);            /* the border frames the whole view */
}

static void
load_clut(struct cdg_screen *s, const unsigned char *d, int first)
{
    int i;

    /*
     * EIGHT ENTRIES PER PACKET, two bytes each, six bits apiece:
     *
     *     byte 0   --rrrrgg      high two bits unused
     *     byte 1   --ggbbbb
     *
     * so green is SPLIT across the pair. Getting that wrong gives a
     * picture with plausible but wrong colours, which is the kind of
     * bug that survives a casual look.
     */
    for (i = 0; i < 8; i++) {
        unsigned char hi = SIX(d[i * 2]);
        unsigned char lo = SIX(d[i * 2 + 1]);
        unsigned short r = (unsigned short)((hi >> 2) & 0x0f);
        unsigned short g = (unsigned short)(((hi & 0x03) << 2)
                                          | ((lo >> 4) & 0x03));
        unsigned short b = (unsigned short)(lo & 0x0f);

        s->clut[first + i] = (unsigned short)((r << 8) | (g << 4) | b);
    }
    /*
     * A PALETTE CHANGE REDRAWS EVERYTHING. The pixels hold INDICES,
     * so recolouring the table changes what is already on screen -
     * which is how these discs fade - and every tile must be resent.
     */
    cdg_mark_all(s);
}

static void
tile_block(struct cdg_screen *s, const unsigned char *d, int xor_mode)
{
    int col0 = SIX(d[0]) & 0x0f;
    int col1 = SIX(d[1]) & 0x0f;
    int row  = SIX(d[2]) & 0x1f;
    int col  = SIX(d[3]) & 0x3f;
    int y;

    /*
     * BOUNDS FIRST. row and col come off the disc and index straight
     * into the pixel array; a scratched sector can carry anything.
     */
    if (row >= CDG_ROWS || col >= CDG_COLS)
        return;

    for (y = 0; y < CDG_TILE_H; y++) {
        unsigned char bits = SIX(d[4 + y]);
        int x;

        for (x = 0; x < CDG_TILE_W; x++) {
            /* BIT 5 IS THE LEFTMOST PIXEL - six pixels in six bits,
             * most significant first. */
            int on = (bits >> (5 - x)) & 1;
            int px = col * CDG_TILE_W + x;
            int py = row * CDG_TILE_H + y;
            int at = py * CDG_WIDTH + px;
            int colour = on ? col1 : col0;

            if (xor_mode)
                s->pixel[at] = (unsigned char)(s->pixel[at] ^ colour);
            else
                s->pixel[at] = (unsigned char)colour;
        }
    }
    /* EXACTLY ONE TILE - this is the precision the row spans exist
     * for, and the whole reason a bounding box was not enough. */
    /*
     * ONE TILE, WIDENED BY THE SCROLL OFFSET.
     *
     * With h_offset or v_offset non-zero the picture is displaced,
     * so a source tile straddles TWO screen tiles - part of it
     * shows at (row, col) and the rest at the neighbour it has
     * slid towards. Marking only (row, col) leaves that sliver
     * stale, which on a scrolling disc is a ragged edge following
     * the text.
     *
     * The extra tile costs nothing when the offsets are 0, which
     * is every disc that does not scroll.
     */
    cdg_mark_tiles(s,
                   (s->v_offset > 0) ? row - 1 : row,
                   (s->h_offset > 0) ? col - 1 : col,
                   row + 1, col + 1);
}

static void
transparent(struct cdg_screen *s, const unsigned char *d)
{
    s->transparent = SIX(d[0]) & 0x0f;
    cdg_mark_all(s);            /* changes how every pixel resolves */
}

static void
scroll(struct cdg_screen *s, const unsigned char *d, int copy)
{
    int colour = SIX(d[0]) & 0x0f;
    int hcmd   = (SIX(d[1]) >> 4) & 0x03;
    int hoff   =  SIX(d[1])       & 0x07;
    int vcmd   = (SIX(d[2]) >> 4) & 0x03;
    int voff   =  SIX(d[2])       & 0x0f;
    int dx = 0, dy = 0;

    /*
     * TWO THINGS IN ONE INSTRUCTION. The low bits are a fine OFFSET
     * (0-5 across, 0-11 down) that shifts where the whole picture
     * sits; the command bits ask for a coarse scroll by a whole tile.
     */
    /*
     * AN OFFSET CHANGE MOVES THE WHOLE PICTURE, so it must mark -
     * and until 2026-09-29 it did not, because the renderer redrew
     * everything on every poll and nothing had to say so.
     *
     * IT IS THE COMMON CASE, NOT AN EDGE ONE. On the Hendrix disc,
     * 577 of 684 scroll instructions change ONLY the offset: the
     * picture slides one pixel at a time through h_offset, and
     * every sixth step becomes a coarse SCROLL_COPY by a whole tile
     * with the offset reset to 0. Marking only the coarse step
     * shows one jump in six and leaves the five between it stale.
     */
    if (hoff < CDG_TILE_W && hoff != s->h_offset) {
        s->h_offset = hoff;
        cdg_mark_all(s);
    }
    if (voff < CDG_TILE_H && voff != s->v_offset) {
        s->v_offset = voff;
        cdg_mark_all(s);
    }

    if (hcmd == 1)      dx =  CDG_TILE_W;
    else if (hcmd == 2) dx = -CDG_TILE_W;
    if (vcmd == 1)      dy =  CDG_TILE_H;
    else if (vcmd == 2) dy = -CDG_TILE_H;

    if (dx != 0 || dy != 0) {
        static unsigned char tmp[CDG_WIDTH * CDG_HEIGHT];
        int y;

        memcpy(tmp, s->pixel, sizeof tmp);

        for (y = 0; y < CDG_HEIGHT; y++) {
            int x;

            for (x = 0; x < CDG_WIDTH; x++) {
                int sx = x - dx;
                int sy = y - dy;
                unsigned char v;

                if (copy) {
                    /* WRAP. A negative modulus is implementation
                     * defined in C89, so bring it positive first. */
                    while (sx < 0) sx += CDG_WIDTH;
                    while (sy < 0) sy += CDG_HEIGHT;
                    sx %= CDG_WIDTH;
                    sy %= CDG_HEIGHT;
                    v = tmp[sy * CDG_WIDTH + sx];
                } else if (sx < 0 || sx >= CDG_WIDTH ||
                           sy < 0 || sy >= CDG_HEIGHT) {
                    v = (unsigned char) colour;
                } else {
                    v = tmp[sy * CDG_WIDTH + sx];
                }

                s->pixel[y * CDG_WIDTH + x] = v;
            }
        }
        cdg_mark_all(s);        /* the whole picture moved */
    }
}

/* ------------------------------------------------------------------ */

int
cdg_packet(struct cdg_screen *s, const unsigned char *pkt)
{
    const unsigned char *d;
    int inst;

    if (s == NULL || pkt == NULL)
        return 0;

    if (SIX(pkt[0]) != CDG_COMMAND)
        return 0;               /* not a graphics packet */

    inst = SIX(pkt[1]);
    d    = pkt + 4;

    switch (inst) {
    case CDG_MEMORY_PRESET:  memory_preset(s, d);      return 1;
    case CDG_BORDER_PRESET:  border_preset(s, d);      return 1;
    case CDG_TILE_BLOCK:     tile_block(s, d, 0);      return 1;
    case CDG_TILE_BLOCK_XOR: tile_block(s, d, 1);      return 1;
    case CDG_LOAD_CLUT_LO:   load_clut(s, d, 0);       return 1;
    case CDG_LOAD_CLUT_HI:   load_clut(s, d, 8);       return 1;
    case CDG_TRANSPARENT:    transparent(s, d);        return 1;
    case CDG_SCROLL_PRESET:  scroll(s, d, 0);          return 1;
    case CDG_SCROLL_COPY:    scroll(s, d, 1);          return 1;
    default:
        /* IGNORED, NOT AN ERROR - cdg.h says why: the real rip
         * carries three singleton instructions that are read errors,
         * and stopping at one would render nothing. */
        return 0;
    }
}

int
cdg_sector(struct cdg_screen *s, const unsigned char *sub96, int layout)
{
    unsigned char packs[CDTEXT_RW_OUT];
    int i, n = 0;

    if (s == NULL || sub96 == NULL)
        return 0;

    if (layout == CDG_SUB_RW96) {
        /* ALREADY DEINTERLEAVED: four 24-byte packets, no transform.
         * cdg.h has the measurement that says this layout exists. */
        for (i = 0; i < 96 / CDG_PACKET_SIZE; i++)
            n += cdg_packet(s, sub96 + i * CDG_PACKET_SIZE);
        return n;
    }

    /* THE SHARED TRANSFORM. CD-TEXT reads the same 72 bytes as four
     * 18-byte packs; we read three of 24. */
    cdtext_rw_unpack(sub96, packs);

    for (i = 0; i < CDG_PACKETS_PER_SEC; i++)
        n += cdg_packet(s, packs + i * CDG_PACKET_SIZE);

    return n;
}

int
cdg_sniff(const unsigned char *sub, int n_sectors)
{
    int i, direct = 0, unpacked = 0;

    if (sub == NULL || n_sectors <= 0)
        return CDG_SUB_PW96_INTERLEAVED;

    for (i = 0; i < n_sectors; i++) {
        const unsigned char *p = sub + i * VDISC_SUB_SIZE;
        unsigned char packs[CDTEXT_RW_OUT];
        int k;

        for (k = 0; k < 96; k += CDG_PACKET_SIZE)
            if ((p[k] & 0x3f) == CDG_COMMAND)
                direct++;

        cdtext_rw_unpack(p, packs);
        for (k = 0; k < CDTEXT_RW_OUT; k += CDG_PACKET_SIZE)
            if ((packs[k] & 0x3f) == CDG_COMMAND)
                unpacked++;
    }

    /*
     * WHICHEVER FOUND MORE. On the two real discs the margin is not
     * close - 69.5% against 0.0% one way, 425 against 11 the other -
     * so a tie means neither layout is carrying graphics and the
     * answer does not matter.
     */
    return (direct > unpacked) ? CDG_SUB_RW96 : CDG_SUB_PW96_INTERLEAVED;
}

unsigned short
cdg_rgb_at(const struct cdg_screen *s, int x, int y)
{
    int idx;

    if (s == NULL)
        return 0;

    if (x < 0 || x >= CDG_WIDTH || y < 0 || y >= CDG_HEIGHT)
        return s->clut[s->border & 0x0f];

    idx = s->pixel[y * CDG_WIDTH + x] & 0x0f;
    return s->clut[idx];
}
