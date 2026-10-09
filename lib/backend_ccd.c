/*
 * backend_ccd.c - CloneCD .ccd + .img backend.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * The .ccd is an INI-shaped control file. It is preferred over a .cue for
 * CloneCD sets because it is the authoritative source: .cue files in such
 * sets are usually machine-converted from the .ccd and lose the lead-in
 * entries and the explicit Control nibbles.
 *
 * Layout:
 *
 *   [Entry N]           one per TOC entry
 *   Session=n           which session's lead-in or track this is
 *   Point=0xa0          lead-in: first track number in PMin
 *   Point=0xa1          lead-in: last track number in PMin
 *   Point=0xa2          lead-in: lead-out LBA in PLBA
 *   Point=0x01..0x63    a track; start LBA in PLBA, flags in Control
 *
 *   [TRACK n]
 *   MODE=1              1 = Mode-1 data, 2 = Mode-2 (XA) data, 0 = audio
 *
 * Control nibble bits: 0x01 pre-emphasis, 0x02 copy permitted,
 * 0x04 data track, 0x08 four-channel audio - all four kept, as
 * VDISC_CTRL_* in vdisc.h (design/27 4b dropped two of them).
 *
 * NOTE: CloneCD is a Windows tool and every .ccd has CRLF line endings.
 * Failing to strip the CR makes every hex value parse as zero.
 *
 * C89 / GCC 2.95 clean.
 */

/*
 * CD-TEXT IS NOT PARSED FROM `[CDText]', AND THAT IS A DECISION -
 * 2026-09-26.
 *
 * CCD carries the lead-in's CD-TEXT as a `[CDText]' section with a
 * `CDTextLength=' count, and `cdtext.h' already handles the shape:
 * "CCD's [CDText] block holds packs that are ALREADY unpacked, so
 * they feed cdtext_parse() directly". So the parser exists and only
 * the extraction is missing.
 *
 * IT IS UNBUILT BECAUSE NO DISC HERE CAN TEST IT. The one CCD in
 * `ExampleCDs/' - Information Society, which is a CD+G reference
 * disc - says `CDTextLength=0'. Writing an extractor against
 * nothing is how this project acquires code that looks right and
 * has never run.
 *
 * AND THE USER'S DISCS WOULD NOT EXERCISE IT EITHER (2026-09-26):
 * the ones with CD-TEXT "would need ripping and were made from cue
 * sheets anyways" - and the CUE path IS built, `backend_cue.c:165',
 * parsing TITLE, PERFORMER, SONGWRITER, COMPOSER and ARRANGER at
 * both disc and track level into `img->text'.
 *
 * SO A .ccd RIP WOULD HAVE TO BE MADE DELIBERATELY to reach this
 * code, and nothing wants it. If one ever appears, the work is an
 * hour: read the count, read that many bytes of packs, hand them to
 * cdtext_parse().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "image.h"

#define CCD_MAX_ENTRIES 256

#define POINT_FIRST_TRACK   0xA0
#define POINT_LAST_TRACK    0xA1
#define POINT_LEADOUT       0xA2

struct ccd_entry {
    int valid;
    int session;        /* Session= on the entry; 0 if absent (then 1) */
    int point;
    int control;
    int adr;
    int plba;
    int pmin;
    int psec;
    int pframe;
};

/* Strip CR/LF and trailing blanks in place. */
static void
chomp(char *s)
{
    int n = (int) strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static void
trim_lead(char **s)
{
    while (**s == ' ' || **s == '\t')
        (*s)++;
}

/* CCD values are decimal or 0x-prefixed hex. */
static int
ccd_num(const char *v)
{
    while (*v == ' ' || *v == '\t')
        v++;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        return (int) strtol(v + 2, (char **) NULL, 16);
    return (int) strtol(v, (char **) NULL, 10);
}

static int
ends_with_ci(const char *s, const char *suffix)
{
    int ls = (int) strlen(s);
    int lx = (int) strlen(suffix);
    int i;

    if (lx > ls)
        return 0;
    for (i = 0; i < lx; i++) {
        if (tolower((unsigned char) s[ls - lx + i]) !=
            tolower((unsigned char) suffix[i]))
            return 0;
    }
    return 1;
}

static int
ccd_probe(const char *path)
{
    return ends_with_ci(path, ".ccd");
}

/* Find the data file that goes with a .ccd. */
static int
find_image_file(const char *ccd_path, char *out, size_t outsz)
{
    static const char *exts[] = { "img", "IMG", "bin", "BIN" };
    char base[1024];
    int i, n;
    FILE *fp;

    strncpy(base, ccd_path, sizeof base - 1);
    base[sizeof base - 1] = '\0';

    n = (int) strlen(base);
    if (n > 4 && ends_with_ci(base, ".ccd"))
        base[n - 4] = '\0';

    for (i = 0; i < (int) (sizeof exts / sizeof exts[0]); i++) {
        char cand[1024];
        sprintf(cand, "%.1000s.%s", base, exts[i]);
        fp = fopen(cand, "rb");
        if (fp) {
            fclose(fp);
            if (strlen(cand) >= outsz)
                return -1;
            strcpy(out, cand);
            return 0;
        }
    }
    return -1;
}

static int
ccd_open(struct vdisc_image *img, const char *path)
{
    FILE *fp;
    char line[512];
    struct ccd_entry entries[CCD_MAX_ENTRIES];
    int cur = -1;
    int in_entry = 0;
    int i, t;
    int first, last, leadout;
    int n_sessions;
    int sess_first[VDISC_MAX_TRACKS + 2];   /* per session: 0xA0's PMin  */
    int sess_last[VDISC_MAX_TRACKS + 2];    /* per session: 0xA1's PMin  */
    int sess_leadout[VDISC_MAX_TRACKS + 2]; /* per session: 0xA2's PLBA  */
    int track_mode[VDISC_MAX_TRACKS + 1];
    int cur_track = -1;
    long sectors;

    memset(entries, 0, sizeof entries);
    for (i = 0; i <= VDISC_MAX_TRACKS; i++)
        track_mode[i] = -1;
    for (i = 0; i <= VDISC_MAX_TRACKS + 1; i++)
        sess_first[i] = sess_last[i] = sess_leadout[i] = -1;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "vdiscd: %s: cannot open\n", path);
        return -1;
    }

    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        char *eq;

        chomp(p);
        trim_lead(&p);

        if (*p == ';' || *p == '\0')
            continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close)
                *close = '\0';
            p++;

            in_entry = 0;
            cur_track = -1;

            if (strncmp(p, "Entry ", 6) == 0) {
                cur = atoi(p + 6);
                if (cur >= 0 && cur < CCD_MAX_ENTRIES) {
                    in_entry = 1;
                    entries[cur].valid = 1;
                }
            } else if (strncmp(p, "TRACK ", 6) == 0) {
                cur_track = atoi(p + 6);
                if (cur_track < 0 || cur_track > VDISC_MAX_TRACKS)
                    cur_track = -1;
            }
            continue;
        }

        eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        chomp(p);

        if (in_entry && cur >= 0 && cur < CCD_MAX_ENTRIES) {
            struct ccd_entry *e = &entries[cur];
            if (strcmp(p, "Point") == 0)        e->point   = ccd_num(eq + 1);
            else if (strcmp(p, "Session") == 0) e->session = ccd_num(eq + 1);
            else if (strcmp(p, "Control") == 0) e->control = ccd_num(eq + 1);
            else if (strcmp(p, "ADR") == 0)     e->adr     = ccd_num(eq + 1);
            else if (strcmp(p, "PLBA") == 0)    e->plba    = ccd_num(eq + 1);
            else if (strcmp(p, "PMin") == 0)    e->pmin    = ccd_num(eq + 1);
            else if (strcmp(p, "PSec") == 0)    e->psec    = ccd_num(eq + 1);
            else if (strcmp(p, "PFrame") == 0)  e->pframe  = ccd_num(eq + 1);
        } else if (cur_track > 0) {
            if (strcmp(p, "MODE") == 0)
                track_mode[cur_track] = ccd_num(eq + 1);
        }
    }
    fclose(fp);

    /*
     * THE LEAD-IN POINTS ARE PER SESSION - design/09's multi-session
     * defect 1, fixed 2026-09-15 (design/29 deliverable 4). A Blue Book
     * disc carries 0xA0/0xA1/0xA2 in EVERY session's lead-in: session
     * 1 says "tracks 1..2, lead-out 6000", session 2 says "track 3..3,
     * lead-out 10000". Taking them globally, last-write-wins, produced
     * "implausible track range 3..2" and the disc did not open at all.
     *
     * So: the disc's first track is the LOWEST session's 0xA0, its
     * last track the HIGHEST 0xA1 seen (or the highest track point, if
     * a session's lead-in lacks 0xA1), and its lead-out the highest
     * session's 0xA2. Each session's own 0xA2 is where ITS last track
     * ends - not the next session's first track, 750 or 11400 sectors
     * later across the inter-session gap.
     *
     * An entry with no Session= is session 1, which is what a
     * single-session .ccd has on every line anyway.
     */
    n_sessions = 0;
    for (i = 0; i < CCD_MAX_ENTRIES; i++) {
        int sn;
        if (!entries[i].valid)
            continue;
        sn = entries[i].session > 0 ? entries[i].session : 1;
        if (sn > VDISC_MAX_TRACKS + 1) {
            fprintf(stderr, "vdiscd: %s: implausible session number %d\n",
                    path, sn);
            return -1;
        }
        if (sn > n_sessions)
            n_sessions = sn;
        if (entries[i].point == POINT_FIRST_TRACK) sess_first[sn]   = entries[i].pmin;
        if (entries[i].point == POINT_LAST_TRACK)  sess_last[sn]    = entries[i].pmin;
        if (entries[i].point == POINT_LEADOUT)     sess_leadout[sn] = entries[i].plba;
    }

    first = last = leadout = -1;
    for (i = 1; i <= n_sessions; i++) {
        if (sess_first[i] >= 0 && first < 0)
            first = sess_first[i];
        if (sess_last[i] > last)
            last = sess_last[i];
        if (sess_leadout[i] > leadout)
            leadout = sess_leadout[i];
    }
    /* a session's lead-in without 0xA1: the highest track point stands
     * in for it, so a hand-made set still opens */
    for (i = 0; i < CCD_MAX_ENTRIES; i++) {
        if (entries[i].valid && entries[i].point >= 1 &&
            entries[i].point <= VDISC_MAX_TRACKS && entries[i].point > last)
            last = entries[i].point;
    }

    if (first < 0 || last < 0 || leadout < 0) {
        fprintf(stderr, "vdiscd: %s: missing lead-in entries "
                        "(0xA0/0xA1/0xA2)\n", path);
        return -1;
    }
    /*
     * `first < 1' WAS MISSING - design/26 B9, fixed 2026-09-14. Track
     * numbers start at 1, and track[] holds VDISC_MAX_TRACKS entries
     * indexed from 0; a file whose 0xA0 entry says PMin=0 with points
     * 0..99 passed `last <= 99', built 100 tracks, and the hundredth
     * went one past the array into `backend' - a garbage pointer that
     * segfaulted on close. Only a hostile or broken file does it (no
     * tool writes PMin=0), but a daemon should refuse it with a
     * message rather than die.
     */
    if (first < 1 || last < first || last > VDISC_MAX_TRACKS) {
        fprintf(stderr, "vdiscd: %s: implausible track range %d..%d\n",
                path, first, last);
        return -1;
    }

    /* Build the track table. */
    img->n_tracks = 0;
    for (t = first; t <= last; t++) {
        struct ccd_entry *e = NULL;
        struct vdisc_track *tr;

        for (i = 0; i < CCD_MAX_ENTRIES; i++) {
            if (entries[i].valid && entries[i].point == t) {
                e = &entries[i];
                break;
            }
        }
        if (!e) {
            fprintf(stderr, "vdiscd: %s: no TOC entry for track %d\n", path, t);
            return -1;
        }

        tr = &img->track[img->n_tracks++];
        tr->num            = t;
        tr->session        = e->session > 0 ? e->session : 1;
        tr->start_lba      = e->plba;
        tr->is_data        = (e->control & VDISC_CTRL_DATA) ? 1 : 0;
        /* The whole nibble, less the data bit, which is_data carries.
         * All four came off the disc's Q channel; two used to be
         * dropped here (design/27 4b). */
        tr->control        = e->control & (VDISC_CTRL_PRE_EMPHASIS |
                                           VDISC_CTRL_COPY_PERMIT |
                                           VDISC_CTRL_FOUR_CHANNEL);

        /* CloneCD images are raw throughout. [TRACK n] MODE is a
         * secondary source for the data/audio split; if it disagrees
         * with the Control nibble, trust Control (it comes from the
         * actual TOC) but say so. */
        if (track_mode[t] >= 0) {
            int mode_says_data = (track_mode[t] != 0);
            if (mode_says_data != tr->is_data) {
                fprintf(stderr, "vdiscd: warning: %s: track %d Control says "
                                "%s but MODE=%d says %s; trusting Control\n",
                        path, t, tr->is_data ? "data" : "audio",
                        track_mode[t], mode_says_data ? "data" : "audio");
            }
        }
        /* MODE=2 is CD-ROM XA; the form is then per sector, read from
         * each sector's own subheader (design/27 3c). Anything else
         * that Control calls data is Mode 1. */
        if (!tr->is_data)
            tr->sector_form = VDISC_FORM_AUDIO_2352;
        else if (track_mode[t] == 2)
            tr->sector_form = VDISC_FORM_MODE2;
        else
            tr->sector_form = VDISC_FORM_MODE1_2352;
    }

    /*
     * Lengths run to the next track IN THE SAME SESSION; the last track
     * of a session runs to THAT session's lead-out. On bluebook that
     * makes track 2 end at 6000, not at track 3's 6750 - the 750
     * between are the inter-session gap, which no track owns and no
     * read or play reaches.
     */
    for (i = 0; i < img->n_tracks; i++) {
        int end;
        int sn = img->track[i].session;

        if (i + 1 < img->n_tracks && img->track[i + 1].session == sn)
            end = img->track[i + 1].start_lba;
        else if (sess_leadout[sn] >= 0)
            end = sess_leadout[sn];
        else
            end = leadout;
        if (i + 1 < img->n_tracks && img->track[i + 1].session < sn) {
            fprintf(stderr, "vdiscd: %s: track %d is in session %d but the "
                            "next track is in session %d\n", path,
                    img->track[i].num, sn, img->track[i + 1].session);
            return -1;
        }
        img->track[i].length = end - img->track[i].start_lba;
        if (img->track[i].length <= 0) {
            fprintf(stderr, "vdiscd: %s: track %d has non-positive length %d\n",
                    path, img->track[i].num, img->track[i].length);
            return -1;
        }
    }

    img->first_track  = first;
    img->last_track   = last;
    img->leadout_lba  = leadout;
    img->n_sessions   = n_sessions > 0 ? n_sessions : 1;

    strncpy(img->desc, path, sizeof img->desc - 1);
    img->desc[sizeof img->desc - 1] = '\0';

    /* A `.sub' OR `.cdg' BESIDE THE `.ccd' - the CloneCD layout is
     * THREE files and this is the third. Silent when absent. */
    vdisc_image_find_sub(img, path);

    {
        char imgpath[1024];
        int k;

        if (find_image_file(path, imgpath, sizeof imgpath) < 0) {
            fprintf(stderr, "vdiscd: %s: no .img/.bin found alongside\n", path);
            return -1;
        }
        k = vdisc_image_add_file(img, imgpath);
        if (k < 0)
            return -1;
        /* CloneCD images are raw throughout: one file, one stride. */
        vdisc_image_set_stride(img, k, VDISC_RAW_SECTOR);
        sectors = img->file[k].n_sectors;

        if (sectors < leadout) {
            fprintf(stderr, "vdiscd: %s: image holds %ld sectors but lead-out "
                            "is %d - image is too short\n",
                    imgpath, sectors, leadout);
            return -1;
        }
        if (vdisc_image_add_extent(img, k, 0, 0, (int) sectors) < 0)
            return -1;
    }

    return 0;
}

static int
ccd_read_data(struct vdisc_image *img, int lba, int n, void *buf)
{
    return vdisc_image_read_data(img, lba, n, buf);
}

static int
ccd_read_audio(struct vdisc_image *img, int lba, int n, void *buf)
{
    return vdisc_image_read_audio(img, lba, n, buf);
}

static void
ccd_close(struct vdisc_image *img)
{
    /* nothing backend-specific to release */
    (void) img;
}

const struct vdisc_backend vdisc_backend_ccd = {
    "ccd",
    ccd_probe,
    ccd_open,
    ccd_read_data,
    ccd_read_audio,
    ccd_close
};
