/*
 * cdtext.c - CD-TEXT packs. READ cdtext.h FIRST; it has the layout,
 * where the format was measured from, and which references were read.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <string.h>

#include "cdtext.h"

/* ------------------------------------------------------------------ */
/* R-W unpack - shared with CD+G                                      */
/* ------------------------------------------------------------------ */

void
cdtext_rw_unpack(const unsigned char *in, unsigned char *out)
{
    int i, o = 0;

    if (in == NULL || out == NULL)
        return;

    /*
     * SIX BITS PER INPUT BYTE, so four in make three out. The top two
     * bits of each input byte are not ours - the P and Q channels live
     * there - which is why every term is masked to 0x3f's worth before
     * it is shifted into place.
     */
    for (i = 0; i + 3 < CDTEXT_RW_IN; i += 4) {
        out[o++] = (unsigned char)(((in[i]     << 2) & 0xfc)
                                 | ((in[i + 1] >> 4) & 0x03));
        out[o++] = (unsigned char)(((in[i + 1] << 4) & 0xf0)
                                 | ((in[i + 2] >> 2) & 0x0f));
        out[o++] = (unsigned char)(((in[i + 2] << 6) & 0xc0)
                                 |  (in[i + 3]       & 0x3f));
    }
}

/* ------------------------------------------------------------------ */
/* CRC                                                                */
/* ------------------------------------------------------------------ */

unsigned short
cdtext_crc(const unsigned char *data, size_t len)
{
    unsigned short crc = 0;
    size_t i;
    int b;

    if (data == NULL)
        return 0;

    /*
     * CCITT, polynomial 0x1021, init 0, and the result INVERTED.
     *
     * The inversion is not decoration and it was not guessed: without
     * it none of the fixture's 96 packs match, with it all 96 do.
     * cdtext.h records how that was established.
     */
    for (i = 0; i < len; i++) {
        crc ^= (unsigned short)(data[i] << 8);
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (unsigned short)((crc << 1) ^ 0x1021);
            else
                crc = (unsigned short)(crc << 1);
        }
    }

    return (unsigned short)(crc ^ 0xffff);
}

int
cdtext_pack_ok(const unsigned char *pack)
{
    unsigned short want, got;

    if (pack == NULL)
        return 0;

    want = (unsigned short)((pack[16] << 8) | pack[17]);
    got  = cdtext_crc(pack, 16);

    return (want == got) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

/*
 * WHERE DOES THIS PACK TYPE'S TEXT GO? Returns the field within an
 * entry, or NULL for a pack type that carries something other than a
 * string (the TOC packs, SIZE_INFO) which this reader does not use.
 */
static char *
field_for(struct cdtext_entry *e, unsigned char type)
{
    switch (type) {
    case CDTEXT_TITLE:      return e->title;
    case CDTEXT_PERFORMER:  return e->performer;
    case CDTEXT_SONGWRITER: return e->songwriter;
    case CDTEXT_COMPOSER:   return e->composer;
    case CDTEXT_ARRANGER:   return e->arranger;
    case CDTEXT_MESSAGE:    return e->message;
    case CDTEXT_UPC_ISRC:   return e->isrc;
    default:                return NULL;
    }
}

/* Append one byte to a NUL-terminated field, silently dropping
 * anything past the end rather than overrunning it. */
static void
field_add(char *field, int ch)
{
    size_t n;

    if (field == NULL)
        return;
    n = strlen(field);
    if (n + 1 >= CDTEXT_STR_MAX)
        return;
    field[n]     = (char) ch;
    field[n + 1] = '\0';
}

/*
 * REFUSE THE WHOLE BLOCK. Called when a disc declares text this file
 * cannot decode - by its character code, or by a DBCC bit.
 *
 * EVERYTHING TAKEN SO FAR GOES. Packs arrive in sequence order and
 * the signal can come after text we have already appended, so
 * dropping only what follows would leave a half-built string that
 * looks like a complete one.
 */
static void
cdtext_refuse(struct cdtext *out)
{
    int t;

    memset(&out->disc, 0, sizeof out->disc);
    for (t = 0; t < CDTEXT_MAX_TRACKS; t++)
        memset(&out->track[t], 0, sizeof out->track[t]);
    out->present = 0;
    out->usable  = 0;
}

int
cdtext_parse(const unsigned char *data, size_t n_packs,
             struct cdtext *out)
{
    size_t i;
    int    block0 = -1;

    if (data == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof *out);
    out->usable = 1;            /* until a SIZE_INFO says otherwise */

    for (i = 0; i < n_packs; i++) {
        const unsigned char *p = data + i * CDTEXT_PACK_SIZE;
        unsigned char type  = p[0];
        unsigned char track = p[1];
        int           block;
        int           j;
        struct cdtext_entry *e;
        char                *field;

        /* BIT 0x80 MARKS A PACK. Anything without it is padding or a
         * gap, not a damaged pack, so it is skipped silently and does
         * not count against bad_crc. */
        if ((type & 0x80) == 0)
            continue;

        /*
         * SIZE_INFO CARRIES THE CHARACTER CODE, in its first data
         * byte. Read it before anything else uses the text: a block
         * declaring Shift-JIS must not be reassembled as 8-bit, and
         * the packs are in sequence order so 0x8f may arrive after
         * text we have already taken.
         */
        if (type == CDTEXT_SIZE_INFO && cdtext_pack_ok(p)) {
            out->charcode = p[4];
            if (out->charcode != CDTEXT_CHAR_8859_1 &&
                out->charcode != CDTEXT_CHAR_ASCII) {
                /*
                 * NOT DECODABLE. Mixing is forbidden by the spec, so
                 * no later pack can be in an encoding we handle -
                 * stop rather than skipping this one.
                 */
                cdtext_refuse(out);
                return 0;
            }
            continue;
        }

        if (!cdtext_pack_ok(p)) {
            out->bad_crc++;
            continue;
        }

        /*
         * DBCC - THE SECOND ENCODING SIGNAL, and the one that is per
         * PACK rather than per block. MMC-3 Annex J, Table J.3: bit 7
         * of byte 3 is "the Double Byte Character Code indication...
         * If it is set to 0b, the Single Byte Character Code is
         * used".
         *
         * A WELL-FORMED DISC NEVER REACHES THIS. Its character code
         * in pack 0x8f would have refused the block already; the
         * spec fixes the two together. This catches the disc that
         * sets DBCC while declaring 0x00 - malformed, and exactly the
         * sort of thing that exists.
         *
         * CHECKED AFTER THE CRC, deliberately: a corrupt pack must
         * not be able to blank a disc that is fine. bad_crc already
         * took it, and a flipped bit here would otherwise refuse the
         * whole block on one bad byte.
         *
         * IT REFUSES RATHER THAN SKIPPING THE PACK. Character
         * Position counts double-byte pairs as one, so a string
         * spanning packs cannot be reassembled correctly once any
         * part of it is double-byte - and a title missing its middle
         * is worse than no title.
         */
        if (p[3] & 0x80) {
            cdtext_refuse(out);
            return 0;
        }

        /*
         * FIRST BLOCK ONLY - cdtext.h says why. The block number is
         * BITS 4-6 of byte 3 (Table J.3), which is why bit 7 is
         * masked off here as well as tested above; the low four are a
         * character position we do not need, because strings are
         * reassembled by appending in sequence order rather than by
         * seeking.
         */
        block = (p[3] >> 4) & 0x07;
        if (block0 < 0)
            block0 = block;
        if (block != block0)
            continue;

        if (track >= CDTEXT_MAX_TRACKS)
            continue;

        e = (track == 0) ? &out->disc : &out->track[track];
        field = field_for(e, type);
        if (field == NULL)
            continue;           /* a pack type we do not present */

        /*
         * TWELVE BYTES, AND A NUL ENDS THE STRING RATHER THAN THE
         * PACK. A pack can hold the tail of one string, a separator,
         * and the head of the next - "s\0Song of Jo" in the fixture -
         * so each NUL advances to the next track's entry for this
         * same field.
         */
        for (j = 0; j < CDTEXT_PACK_TEXT; j++) {
            int ch = p[4 + j];

            if (ch == '\0') {
                /* End of this entry's string. The next characters
                 * belong to the next track, which is how a run of
                 * titles is packed. */
                track++;
                if (track >= CDTEXT_MAX_TRACKS)
                    break;
                e = (track == 0) ? &out->disc : &out->track[track];
                field = field_for(e, type);
                if (field == NULL)
                    break;
                continue;
            }
            field_add(field, ch);
        }

        out->present = 1;

        /*
         * COUNT FROM THE PACK'S OWN TRACK FIELD, not from `track' as
         * the loop above left it.
         *
         * That local is ADVANCED past each NUL so a run of titles
         * lands in consecutive entries, so by the end of a pack it
         * points at the track whose string is next - one beyond
         * anything this pack actually filled, and further still for a
         * pack holding several short strings. Counting it gave 14 for
         * a three-track disc.
         */
        if ((int) p[1] > out->n_tracks && p[1] < CDTEXT_MAX_TRACKS)
            out->n_tracks = (int) p[1];
    }

    return 0;
}
