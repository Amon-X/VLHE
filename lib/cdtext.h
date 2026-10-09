/*
 * cdtext.h - CD-TEXT: the 18-byte packs and what they say.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * WHERE IT LIVES ON A DISC. CD-TEXT rides the R-W channels of the
 * subchannel in the LEAD-IN, which is why design/27 section 4a could
 * say both that our images carry the space for it (QUAKE106.sub is a
 * complete 96-byte-per-sector capture) and that they carry none of it
 * (R-W is all zeros on that disc - nothing was ever written there).
 *
 * THIS FILE IS THE PACK LAYER ONLY. Unpacking 96 bytes of R-W into 72
 * bytes of pack data is a separate step shared with CD+G - see
 * cdtext_rw_unpack() below, which is the same transform both formats
 * need. A `.cdt' file, and CCD's [CDText] block, hold packs that are
 * ALREADY unpacked, so they feed cdtext_parse() directly.
 *
 * THE FORMAT, MEASURED RATHER THAN READ. Every claim here was checked
 * against libcdio 2.1.0's test corpus (see below); nothing is taken on
 * trust from a specification we do not have:
 *
 *   18 bytes per pack, no file header, no alignment padding
 *      0  pack type    0x80 + field, so 0x80 TITLE ... 0x8f SIZE_INFO
 *      1  track        0 = the disc itself, else the track number
 *      2  sequence     counts 0,1,2... across the whole block
 *      3  block/char   high bits block number and a double-byte flag,
 *                      low bits the character position within a
 *                      string that started in an earlier pack
 *      4..15  twelve bytes of text
 *      16..17 CRC-16, big endian
 *
 *   STRINGS SPAN PACKS AND ARE NUL-SEPARATED. One pack holds twelve
 *   bytes and a title is often longer, so a string runs into the next
 *   pack and a single pack can hold the tail of one string, a NUL, and
 *   the head of the next. Pack 1 of the fixture is exactly that:
 *   "s\0Song of Jo".
 *
 *   THE CRC IS CCITT WITH init 0 AND xorout 0xFFFF over bytes 0..15.
 *   Established by trying candidates against all 96 packs of the
 *   fixture: 96 of 96 match, 0 fail. A failing CRC means a damaged
 *   pack, which a reader should skip rather than trust - the text is
 *   twelve bytes of arbitrary content and there is no other way to
 *   know it is wrong.
 *
 * REFERENCES, READ AND NOT COPIED - the practice this project uses for
 * every reference tree (CLAUDE.md section 5):
 *
 *   libcdio-2.1.0/     GPL-3.0. `lib/driver/cdtext.c' is a complete
 *                      reader and `example/cdtext-raw.c' parses the
 *                      exact `.cdt' layout above.
 *                      `include/cdio/cdtext.h' enumerates the fields,
 *                      which are the SPEC's numbers rather than
 *                      anyone's invention: TITLE 0, PERFORMER 1,
 *                      SONGWRITER 2, ... ISRC 6, GENRE 8, DISCID 9.
 *                      ITS TEST DATA IS OUR ORACLE - `test/data/
 *                      cdtext.cdt' with `test/cdtext.right' as the
 *                      expected decode, two languages and non-ASCII
 *                      German, plus a second independently produced
 *                      `cdtext-libburnia.cdt'.
 *
 *   vcd/contrib/refs/cdemu/   `cdemu-daemon/src/device-recording.c'
 *                      has the R-W unpack on the WRITING side, and a
 *                      pack loop that checks bit 0x80 for validity.
 *
 * WE SHIP BSD-3. Both references are read for the format and cited for
 * the reader who wants to check us; no code is taken from either.
 *
 * C89 / GCC 2.95 clean.
 */

#ifndef _VDISC_CDTEXT_H
#define _VDISC_CDTEXT_H

#include <stddef.h>

/* One pack on the wire. */
#define CDTEXT_PACK_SIZE    18
#define CDTEXT_PACK_TEXT    12      /* bytes 4..15                     */

/* R-W: 96 six-bit symbols per sector unpack to 72 bytes, which is four
 * packs of 18. The same transform CD+G needs. */
#define CDTEXT_RW_IN        96
#define CDTEXT_RW_OUT       72

/*
 * PACK TYPES. 0x80 + the field number, and the field numbers are the
 * specification's - the same values libcdio's header enumerates and
 * the same ones the fixtures carry.
 */
#define CDTEXT_TITLE        0x80
#define CDTEXT_PERFORMER    0x81
#define CDTEXT_SONGWRITER   0x82
#define CDTEXT_COMPOSER     0x83
#define CDTEXT_ARRANGER     0x84
#define CDTEXT_MESSAGE      0x85
#define CDTEXT_DISCID       0x86
#define CDTEXT_GENRE        0x87
#define CDTEXT_TOC          0x88
#define CDTEXT_TOC2         0x89
/*
 * NOT DECODED, AND THE REASONS DIFFER.
 *
 *   0x88/0x89 TOC   the disc's own table of contents. We have the
 *                   real TOC from the image, which is authoritative,
 *                   so this is redundant rather than missing.
 *   0x8d CLOSED     "closed information" - private data a disc may
 *                   carry for the producer's own use. The fixture has
 *                   one reading "This is not ...". Nothing to show.
 *   0x8f SIZE_INFO  THE ONE THAT MATTERS - character code, first and
 *                   last track, and the language of each block. See
 *                   the encoding note above.
 */
#define CDTEXT_CLOSED_INFO  0x8d

/*
 * CHARACTER CODES, from the spec's own table - see the encoding note
 * above. Only the first two are decodable by this file.
 */
#define CDTEXT_CHAR_8859_1  0x00    /* modified; the default        */
#define CDTEXT_CHAR_ASCII   0x01
#define CDTEXT_CHAR_SJIS    0x80    /* double byte - NOT decoded    */
#define CDTEXT_UPC_ISRC     0x8e
#define CDTEXT_SIZE_INFO    0x8f

/* What a caller gets back. Fields are NUL-terminated and empty when
 * the disc did not carry them. */
#define CDTEXT_STR_MAX      160     /* generous: packs chain freely    */
#define CDTEXT_MAX_TRACKS   100

struct cdtext_entry {
    char title[CDTEXT_STR_MAX];
    char performer[CDTEXT_STR_MAX];
    char songwriter[CDTEXT_STR_MAX];
    char composer[CDTEXT_STR_MAX];
    char arranger[CDTEXT_STR_MAX];
    char message[CDTEXT_STR_MAX];
    char isrc[CDTEXT_STR_MAX];      /* per track; UPC/EAN on the disc  */
};

struct cdtext {
    int                 present;    /* did any valid pack decode       */
    /*
     * THE BLOCK'S CHARACTER CODE, from pack 0x8f, or 0 if no
     * SIZE_INFO pack was seen - which is also the default and the
     * common case.
     *
     * `usable' IS THE ONE A CALLER SHOULD TEST. It is 0 when the
     * block declares an encoding this file cannot decode, and then
     * the strings are LEFT EMPTY rather than filled with split
     * double-byte characters. Showing nothing is honest; showing
     * mojibake and calling it a title is not.
     */
    int                 charcode;
    int                 usable;
    int                 n_tracks;   /* highest track seen              */
    int                 bad_crc;    /* packs skipped; 0 on a clean rip */
    struct cdtext_entry disc;
    struct cdtext_entry track[CDTEXT_MAX_TRACKS];
};

/*
 * UNPACK ONE SECTOR'S R-W CHANNELS: 96 bytes in, 72 out.
 *
 * Each input byte carries six significant bits, so four of them pack
 * into three output bytes. SHARED WITH CD+G, which reads the same 72
 * bytes as six 24-byte graphics packets rather than four 18-byte text
 * packs.
 *
 * `in' must hold CDTEXT_RW_IN bytes and `out' CDTEXT_RW_OUT.
 */
void cdtext_rw_unpack(const unsigned char *in, unsigned char *out);

/*
 * IS THIS PACK INTACT? Checks the CRC over bytes 0..15 against the
 * two bytes that follow. Returns 1 for good, 0 for damaged.
 */
int cdtext_pack_ok(const unsigned char *pack);

/*
 * THE CRC ITSELF, exposed because a writer needs it too and a test
 * wants to check it directly.
 */
unsigned short cdtext_crc(const unsigned char *data, size_t len);

/*
 * DECODE A RUN OF PACKS. `data' is n_packs * CDTEXT_PACK_SIZE bytes,
 * already unpacked - a `.cdt' file, CCD's [CDText] block, or the
 * output of cdtext_rw_unpack().
 *
 * DAMAGED PACKS ARE SKIPPED, NOT FATAL. A subchannel rip off a
 * scratched disc will have some, and the rest of the text is still
 * worth having; `bad_crc' counts them so a caller can say so.
 *
 * ONLY THE FIRST BLOCK (language) IS KEPT. A disc may carry up to
 * eight, and the fixture carries English and German - but nothing in
 * this project has anywhere to show a second language yet.
 *
 * AND THE BYTES ARE PASSED THROUGH AS THEY COME, WHICH IS A REAL
 * LIMITATION RATHER THAN A DECISION. An earlier version of this
 * comment called the single-block choice "deliberate" and said
 * nothing about encoding at all, which let a reader assume the text
 * was Latin-1. It is not safe to assume that - the user's point,
 * 2026-09-21: they only know English and deliberately took English
 * examples, so the corpus here proves nothing about anyone else's
 * discs.
 *
 * THE FORMAT SAYS WHICH ENCODING IT IS, IN PACK 0x8f (SIZE_INFO).
 * WE NOW READ IT - see `charcode' and `usable' in struct cdtext.
 *
 * THE TABLE, supplied by the user 2026-09-21. ITS CHAIN, RECORDED
 * BECAUSE THE SPEC IS NOT PUBLIC AND THIS IS THE WHOLE EVIDENCE:
 *
 *   whipper issue 169 (github.com/whipper-team/whipper/issues/169)
 *     -> a cdrdao mailing list post, which quotes the spec directly
 *       -> the Sony/Philips CD-TEXT definition itself
 *
 * SO IT IS A QUOTE OF A QUOTE, not a reading of the document. That
 * is the best available here (see WHERE THE SPEC ITSELF IS, below)
 * and it is worth knowing which it is. Two things argue it is
 * faithful: it is self-consistent with libcdio's independent list,
 * and it carries detail an implementation would have dropped - the
 * "to be defined" wording and the CD EXTRA cross-reference.
 *
 * The table, as quoted:
 *
 *      $00         ISO/IEC 8859-1 (MODIFIED - see the CD EXTRA
 *                  Specification, appendix 1)   <- what we assume
 *      $01         ISO/IEC 646, ASCII (7 bit)
 *      $02 .. $7F  Reserved
 *      $80         Music Shift-JIS Kanji, DOUBLE BYTE
 *      $81         Korean character code (TO BE DEFINED)
 *      $82         Mandarin Chinese character code (TO BE DEFINED)
 *      $83 .. $FF  Reserved
 *
 * TWO THINGS IN THAT TABLE THAT READING IMPLEMENTATIONS DID NOT
 * SETTLE, and both were open questions here an hour before it
 * arrived:
 *
 *   KOREAN AND CHINESE ARE IN THE SPEC, as "to be defined" - so the
 *   CODE POINTS are allocated and the ENCODINGS never were. libcdio
 *   calls them "proposed but never implemented anywhere", which is
 *   right about implementations and understates the spec. A disc
 *   claiming $81 is not malformed; it is using something nobody
 *   finished.
 *
 *   $00 IS *MODIFIED* 8859-1, not plain Latin-1 - the modification
 *   is in the CD EXTRA Specification appendix 1, which this project
 *   does not hold. So even the default case is not quite what a
 *   `char' comparison assumes.
 *
 * AND MIXING IS FORBIDDEN: a disc is Shift-JIS or 8859-1, not both.
 * The spec also fixes packs OUTSIDE $80..$85 at character code $00
 * regardless of what the block declares - so the code only governs
 * the six TEXT pack types, which is exactly the range this file
 * decodes.
 *
 * SHIFT_JIS IS THE ONE THAT BREAKS THINGS, AND IT IS THE ONLY ONE
 * THAT CAN. $81 and $82 are undefined, so no disc can be carrying
 * them meaningfully; $02..$7F and $83..$FF are reserved. That leaves
 * exactly one encoding in the wild that we would mishandle.
 *
 * It is double-byte, so a reader treating it as 8-bit does not merely
 * show the wrong accents - it splits every character and produces
 * nonsense, and our NUL-scanning string reassembly would cut in the
 * middle of a character.
 *
 * WHERE THE SPEC ITSELF IS, since this cannot be looked up here.
 * THERE ARE TWO DOCUMENTS AND THEY ANSWER DIFFERENT HALVES:
 *
 *   THE PACK FORMAT AND THE CHARACTER CODES are the RED BOOK's
 *   (Philips/Sony's own System Description) - licensed, never
 *   freely published. NO PUBLIC DOCUMENT CARRIES THE TABLE ABOVE,
 *   which is why it reaches us as a quote of a quote.
 *
 *   HOW A DRIVE HANDS THE PACKS OVER is MMC's - READ TOC/PMA/ATIP
 *   with Format = 0101b.
 *
 * MMC-2 IS THE WRONG DOCUMENT - the user's correction, 2026-09-21,
 * and it was exactly right. MMC-2 (mmc2r02.txt) defines READ
 * TOC/PMA/ATIP formats 0000b through 0100b and STOPS; its own
 * contents list tables 143, 144, 145, 149 and 150 for those five and
 * none for 0101b. Its single "0101b" string is an unrelated ADR
 * value at :1756. So it predates the command rather than declining
 * to describe it.
 *
 * MMC-3 IS NOW IN THE PROJECT - vcd/contrib/refs/specs/mmc3r10g.pdf
 * and .txt, revision 10g, 12 November 2001, T10/1363-D, 471 pages,
 * fetched 2026-09-21 at the user's suggestion. IT HAS AN ENTIRE
 * CD-TEXT ANNEX, which is more than was expected:
 *
 *   ANNEX J (INFORMATIVE), "CD-TEXT Format in the Lead-in Area"
 *     Table J.1  the 18-byte pack layout, byte by byte
 *     Table J.2  ALL THIRTEEN PACK TYPES, 80h..8Fh
 *     Table J.3  the ID4 byte: DBCC, Block Number, Char Position
 *   5.23.7 + Table 244   READ TOC/PMA/ATIP with Format = 0101b
 *
 * EVERY PACK TYPE THIS FILE DEFINES MATCHES TABLE J.2 EXACTLY,
 * 0x8d "Reserved for content provider only" included. So the pack
 * layer is no longer inferred from implementations - it is confirmed
 * against a standard, and the 96/96 CRC match now has a documented
 * companion rather than standing alone.
 *
 * AND ANNEX J CONFIRMS THE 6-TO-8 UNPACK IN WORDS: raw R-W
 * subchannel, "converted from 6 bits to 8 bits", "4 chunk of 18
 * bytes data" - which is cdtext_rw_unpack() and the four packs
 * cdtext_parse() reads, stated by the spec rather than measured.
 *
 * BUT THE CHARACTER CODE TABLE IS STILL NOT IN IT. Counted with awk
 * across all 471 pages: ZERO hits for 8859, Shift or Kanji. Annex J
 * has three tables and none is a character-code list.
 *
 * MMC-5 (T10/1675D rev 4, 2006) WAS CHECKED TOO, 2026-09-21 - the
 * last revision, so the place the table would be if it ever entered
 * a public document. Also zero for 8859 and Kanji; its 24 "Shift"
 * hits are all DVD "Shifted Middle Area" addressing, and it has no
 * Annex J at all. NOT KEPT - it is a DVD-era document with nothing
 * else for us.
 *
 * WHAT MMC-3 DOES SAY ABOUT ENCODING IS ONE BIT, NOT A TABLE.
 * Table J.3's bit 7 is DBCC, "Double Byte Character Code
 * indication... If it is set to 0b, the Single Byte Character Code
 * is used" - so the standard acknowledges double-byte text and
 * never names which double-byte encoding. Character Position then
 * counts "a set of 2 bytes... at one".
 *
 * THAT BIT IS A SECOND, INDEPENDENT SIGNAL and we do not read it -
 * see the note on refusing, below.
 *
 * SO THE SPLIT IS NOW MEASURED RATHER THAN ASSUMED: MMC-3 has the
 * TRANSPORT and the PACK FORMAT; the Red Book alone has the
 * CHARACTER CODES. IEC 908:1987 is the public standardisation of the
 * Red Book and PREDATES CD-TEXT by a decade, so it would not settle
 * it either.
 *
 * NONE OF THIS BLOCKS US TODAY: we decode packs handed to us by a
 * CUE sheet or an image's subchannel, not by a drive, so the MMC
 * command is not on our path yet.
 *
 * SIZE_INFO ALSO CARRIES the first and last track numbers and the
 * language code of each of the eight blocks (`langcode[8]'), so
 * reading it would also let us say WHICH language is being shown
 * rather than "the first one".
 *
 * WHAT THIS MEANS TODAY: on a Latin-1 or ASCII disc the strings are
 * correct. On a Shift-JIS disc THEY COME BACK EMPTY, with `usable'
 * 0 - we refuse rather than mangle. A caller showing nothing and
 * saying why is honest; one showing split double-byte characters as
 * a track title is not, and the mangled version is also the one a
 * user would report as a bug in the wrong place.
 *
 * DECODING SHIFT-JIS IS STILL NOT BUILT, and refusing is not the
 * same as supporting it. What changed is that the failure is now
 * VISIBLE instead of silent.
 *
 * AND WE REFUSE ON BOTH SIGNALS, not one. MMC-3 Annex J's Table J.3
 * puts a DBCC bit in every pack's ID4 byte, set when that pack's
 * text is double-byte - so a disc can say "double byte" per PACK as
 * well as per BLOCK through the character code in 0x8f.
 *
 * A WELL-FORMED DISC ONLY EVER NEEDS THE FIRST. The spec fixes the
 * two together, so 0x8f refuses the block before any DBCC pack is
 * reached. The second check is for the disc that sets DBCC while
 * declaring character code 0x00 - malformed, and exactly the sort of
 * thing that exists.
 *
 * THE DBCC CHECK SITS AFTER THE CRC TEST, DELIBERATELY. A corrupt
 * pack must not be able to blank a disc that is fine: a flipped bit
 * in byte 3 would otherwise refuse a whole block, where bad_crc
 * already accounts for it. test_cdtext.c has that case.
 *
 * AND IT REFUSES RATHER THAN SKIPPING THE PACK, because Character
 * Position counts a double-byte pair as one character - so a string
 * spanning packs cannot be reassembled once any part of it is
 * double-byte, and a title missing its middle is worse than no
 * title at all.
 *
 * Returns 0 on success, -1 if the arguments are unusable.
 */
int cdtext_parse(const unsigned char *data, size_t n_packs,
                 struct cdtext *out);

#endif /* _VDISC_CDTEXT_H */
