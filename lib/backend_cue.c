/*
 * backend_cue.c - CUE sheet + BIN/IMG backend.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * Used when no .ccd is present. Recognised statements:
 *
 *   FILE "name" BINARY     one or MANY - a Redump rip has one per track
 *   TRACK nn MODE1/2048 | MODE1/2352 | MODE2/2352 | MODE2/2336 | AUDIO | CDG
 *            (MODE2/2352 is CD-ROM XA, Form 1 or 2 decided per sector;
 *             MODE2/2336 the same at a 2336-byte stride; CDG is audio
 *             at a 2448-byte stride, the trailing 96 bytes of raw
 *             subchannel ignored here)
 *   FLAGS PRE DCP 4CH
 *   INDEX 00 mm:ss:ff      the pregap that IS in the file
 *   INDEX 01 mm:ss:ff      the track start
 *   PREGAP / POSTGAP mm:ss:ff   gaps NOT in the file - virtual sectors
 *   REM SESSION n          the track's session (IsoBuster writes it)
 *   TITLE / PERFORMER / SONGWRITER / COMPOSER / ARRANGER /
 *   MESSAGE                CD-TEXT, disc-level before the first
 *                          TRACK and track-level inside one
 *   REM (other) / CATALOG / ISRC             ignored
 *
 * INDEX, PREGAP and POSTGAP MSF are plain base-75 (no 150 offset). An
 * INDEX is FILE-RELATIVE; the disc LBA is the file's base in the disc
 * plus the index plus every virtual gap before it. See msf.h, and
 * test_backend_cue.c for the fixtures that pin the arithmetic down.
 *
 * WHY THE FILE TABLE AND NOT A FILE PER TRACK: on the Video CD in
 * ExampleCDs/ track 1 is 3375 sectors in the TOC and its .bin holds
 * 3226 - the other 149 are track 2's INDEX 00 pregap, stored in track
 * 2's file. So a disc sector is found by LBA through the image's
 * extent table (image.h), never through its track.
 *
 * C89 / GCC 2.95 clean.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "image.h"
#include "msf.h"

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

static int
cue_probe(const char *path)
{
    return ends_with_ci(path, ".cue");
}

/* Map a CUE track-mode keyword to (is_data, sector_form, bytes-per-sector) */
static int
cue_mode(const char *kw, int *is_data, int *form, int *secsize)
{
    if (strcmp(kw, "AUDIO") == 0) {
        *is_data = 0; *form = VDISC_FORM_AUDIO_2352; *secsize = 2352;
        return 0;
    }
    if (strcmp(kw, "MODE1/2352") == 0) {
        *is_data = 1; *form = VDISC_FORM_MODE1_2352; *secsize = 2352;
        return 0;
    }
    if (strcmp(kw, "MODE1/2048") == 0) {
        *is_data = 1; *form = VDISC_FORM_ISO_2048;   *secsize = 2048;
        return 0;
    }
    if (strcmp(kw, "MODE2/2352") == 0) {
        /* CD-ROM XA. The form - and so where the user data sits - is
         * decided per SECTOR by vdisc_read_mode2_2352(); the track only
         * says Mode 2. design/27 3c. */
        *is_data = 1; *form = VDISC_FORM_MODE2; *secsize = 2352;
        return 0;
    }
    if (strcmp(kw, "MODE2/2336") == 0) {
        /* The same XA sectors without sync and header, so a 2336-byte
         * stride; subheader at 0, user data at 8. design/26 B8 measured
         * the drift when this was read at 2352. */
        *is_data = 1; *form = VDISC_FORM_MODE2; *secsize = 2336;
        return 0;
    }
    if (strcmp(kw, "CDG") == 0) {
        /* CD+G: audio, and the rip appends 96 bytes of raw interleaved
         * subchannel to every frame - 2448 per sector. The audio plays
         * from the first 2352; the graphics are phase 2's. */
        *is_data = 0; *form = VDISC_FORM_AUDIO_2352; *secsize = 2448;
        return 0;
    }
    return -1;
}

/*
 * CD-TEXT STRAIGHT OUT OF THE CUE SHEET.
 *
 * A sheet may carry the same seven fields the subchannel packs do,
 * already decoded by whoever wrote it:
 *
 *     TITLE "Test CD-Text Album"
 *     PERFORMER "AI Engineer"
 *     FILE "Silence.wav" WAVE
 *       TRACK 01 AUDIO
 *         TITLE "First Test Track"
 *         PERFORMER "AI Engineer"
 *
 * SCOPE IS POSITIONAL, which is the only subtlety: before the first
 * TRACK the fields describe the DISC, and inside a TRACK they describe
 * that track. Same keyword, different target, decided by how far down
 * the file it is.
 *
 * NO SUBCHANNEL, NO UNPACK, NO CRC - this is the easy half of
 * CD-TEXT, and for a CUE it is the ONLY half: a sheet has no lead-in
 * to read packs from.
 *
 * Returns 1 if it stored something, 0 if the keyword was not ours.
 */
static int
cue_cdtext(struct cdtext *ct, int track, const char *kw, const char *line)
{
    struct cdtext_entry *e;
    char  *field;
    const char *q;
    int    n = 0;

    if (ct == NULL)
        return 0;

    if (track <= 0)
        e = &ct->disc;
    else if (track < CDTEXT_MAX_TRACKS)
        e = &ct->track[track];
    else
        return 0;

    if      (strcmp(kw, "TITLE")      == 0) field = e->title;
    else if (strcmp(kw, "PERFORMER")  == 0) field = e->performer;
    else if (strcmp(kw, "SONGWRITER") == 0) field = e->songwriter;
    else if (strcmp(kw, "COMPOSER")   == 0) field = e->composer;
    else if (strcmp(kw, "ARRANGER")   == 0) field = e->arranger;
    else if (strcmp(kw, "MESSAGE")    == 0) field = e->message;
    else return 0;

    /*
     * THE VALUE IS QUOTED, and a title with a quote in it is not
     * something a CUE can express - so the first quote opens and the
     * next closes, with no escape handling, which is what every other
     * reader of these files does.
     */
    q = strchr(line, '"');
    if (q == NULL)
        return 0;
    q++;

    while (*q != '\0' && *q != '"' && n < CDTEXT_STR_MAX - 1)
        field[n++] = *q++;
    field[n] = '\0';

    if (n > 0) {
        ct->present = 1;
        if (track > ct->n_tracks && track < CDTEXT_MAX_TRACKS)
            ct->n_tracks = track;
    }
    return 1;
}

/* Locate the BIN/IMG named by FILE, falling back to sibling extensions. */
static int
find_data_file(const char *cue_path, const char *named, int allow_fallback,
               char *out, size_t outsz)
{
    static const char *exts[] = { "bin", "BIN", "img", "IMG", "iso", "ISO" };
    char dir[1024];
    char cand[1024];
    char base[1024];
    char *slash;
    FILE *fp;
    int i, n;

    strncpy(dir, cue_path, sizeof dir - 1);
    dir[sizeof dir - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash)
        *(slash + 1) = '\0';
    else
        dir[0] = '\0';

    /* 1. the name the CUE gives us, relative to the CUE's directory */
    if (named && *named) {
        sprintf(cand, "%.500s%.500s", dir, named);
        fp = fopen(cand, "rb");
        if (fp) {
            fclose(fp);
            if (strlen(cand) >= outsz)
                return -1;
            strcpy(out, cand);
            return 0;
        }
    }

    /* 2. same basename as the .cue with a known data extension - for
     *    the FIRST file only. A second FILE that is missing is missing;
     *    the sibling would be the wrong file, silently. */
    if (!allow_fallback)
        return -1;
    strncpy(base, cue_path, sizeof base - 1);
    base[sizeof base - 1] = '\0';
    n = (int) strlen(base);
    if (n > 4 && ends_with_ci(base, ".cue"))
        base[n - 4] = '\0';

    for (i = 0; i < (int) (sizeof exts / sizeof exts[0]); i++) {
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

/* What the CUE says about a track that the track table has no room
 * for: which file it is in, and the four positions that place it. */
struct cue_trk {
    int file;           /* index into img->file                         */
    int idx0;           /* INDEX 00, file-relative, or -1               */
    int idx1;           /* INDEX 01, file-relative, or -1               */
    int pregap;         /* PREGAP sectors, virtual, before the track    */
    int postgap;        /* POSTGAP sectors, virtual, after it           */
};

static int
cue_open(struct vdisc_image *img, const char *path)
{
    FILE *fp;
    char line[512];
    struct cue_trk ct[VDISC_MAX_TRACKS];
    int cur = -1;              /* index into img->track */
    int cur_file = -1;         /* index into img->file  */
    int cur_session = 1;       /* REM SESSION n, applied to later tracks */
    int pending_flags = 0;      /* FLAGS seen before this track's TRACK */
    int i, k;
    int disc;                  /* the running disc LBA during layout   */

    img->n_tracks = 0;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "vdiscd: %s: cannot open\n", path);
        return -1;
    }

    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        char kw[64];

        chomp(p);
        trim_lead(&p);
        if (*p == '\0')
            continue;

        if (sscanf(p, "%63s", kw) != 1)
            continue;

        if (strcmp(kw, "FILE") == 0) {
            char named[512];
            char found[1024];
            char *q1 = strchr(p, '"');

            named[0] = '\0';
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = (size_t) (q2 - q1 - 1);
                    if (len >= sizeof named)
                        len = sizeof named - 1;
                    memcpy(named, q1 + 1, len);
                    named[len] = '\0';
                }
            }
            if (find_data_file(path, named, img->n_files == 0,
                               found, sizeof found) < 0) {
                fprintf(stderr, "vdiscd: %s: cannot find the data file "
                                "(FILE said \"%s\")\n", path, named);
                fclose(fp);
                return -1;
            }
            cur_file = vdisc_image_add_file(img, found);
            if (cur_file < 0) {
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (strcmp(kw, "TRACK") == 0) {
            int num;
            char modekw[64];
            int is_data, form, secsize, rc;

            if (sscanf(p, "%*s %d %63s", &num, modekw) != 2)
                continue;
            for (i = 0; modekw[i]; i++)
                modekw[i] = (char) toupper((unsigned char) modekw[i]);

            rc = cue_mode(modekw, &is_data, &form, &secsize);
            if (rc < 0) {
                fprintf(stderr, "vdiscd: %s: unsupported track mode \"%s\"\n",
                        path, modekw);
                fclose(fp);
                return -1;
            }
            if (cur_file < 0) {
                fprintf(stderr, "vdiscd: %s: TRACK %d before any FILE\n",
                        path, num);
                fclose(fp);
                return -1;
            }
            if (img->n_tracks >= VDISC_MAX_TRACKS) {
                fprintf(stderr, "vdiscd: %s: too many tracks\n", path);
                fclose(fp);
                return -1;
            }

            /* The stride is the FILE's. Its first track sets it; a
             * later track in the same file must agree, or the file
             * cannot be indexed at all. */
            if (img->file[cur_file].secsize == 0) {
                vdisc_image_set_stride(img, cur_file, secsize);
            } else if (img->file[cur_file].secsize != secsize) {
                fprintf(stderr, "vdiscd: %s: track %d wants %d-byte sectors "
                                "but its file already has %d-byte ones\n",
                        path, num, secsize, img->file[cur_file].secsize);
                fclose(fp);
                return -1;
            }

            cur = img->n_tracks++;
            memset(&img->track[cur], 0, sizeof img->track[cur]);
            img->track[cur].num         = num;
            img->track[cur].is_data     = is_data;
            img->track[cur].sector_form = form;
            img->track[cur].start_lba   = -1;
            img->track[cur].control     = pending_flags;
            img->track[cur].session     = cur_session;
            pending_flags = 0;

            ct[cur].file    = cur_file;
            ct[cur].idx0    = -1;
            ct[cur].idx1    = -1;
            ct[cur].pregap  = 0;
            ct[cur].postgap = 0;
            continue;
        }

        if (strcmp(kw, "FLAGS") == 0) {
            /* All three of the CUE's flags are Q-channel control bits,
             * and all three are kept (design/27 4b: DCP and 4CH used
             * to be parsed and then dropped). FLAGS may precede the
             * TRACK it describes, so hold them until one arrives. */
            int f = 0;
            if (strstr(p, "PRE"))  f |= VDISC_CTRL_PRE_EMPHASIS;
            if (strstr(p, "DCP"))  f |= VDISC_CTRL_COPY_PERMIT;
            if (strstr(p, "4CH"))  f |= VDISC_CTRL_FOUR_CHANNEL;
            if (cur >= 0)
                img->track[cur].control |= f;
            else
                pending_flags |= f;
            continue;
        }

        if (strcmp(kw, "INDEX") == 0) {
            int idx, m, s, f;

            if (sscanf(p, "%*s %d %d:%d:%d", &idx, &m, &s, &f) != 4)
                continue;
            if (cur < 0)
                continue;
            if (idx == 0)
                ct[cur].idx0 = vdisc_cue_msf_to_sector(m, s, f);
            else if (idx == 1)
                ct[cur].idx1 = vdisc_cue_msf_to_sector(m, s, f);
            /* higher indices subdivide a track; the TOC has no room
             * for them and nothing here asks */
            continue;
        }

        if (strcmp(kw, "PREGAP") == 0 || strcmp(kw, "POSTGAP") == 0) {
            int m, s, f, len;

            if (sscanf(p, "%*s %d:%d:%d", &m, &s, &f) != 3 || cur < 0)
                continue;
            len = vdisc_cue_msf_to_sector(m, s, f);
            if (len < 0 || len > VDISC_MAX_SECTORS)
                continue;
            if (kw[1] == 'R')
                ct[cur].pregap = len;
            else
                ct[cur].postgap = len;
            continue;
        }

        if (strcmp(kw, "REM") == 0) {
            /*
             * REM SESSION n - the one REM this parser reads. IsoBuster
             * and some others write it; it is the only way a CUE says
             * which session a track is in. A CUE has NO per-session
             * lead-out, so the last track of a session runs to the next
             * session's first track here - CCD is the format for a
             * multi-session disc (design/27 section 2) and this is why.
             */
            char w[64];
            int n;
            if (sscanf(p, "%*s %63s %d", w, &n) == 2 &&
                strcmp(w, "SESSION") == 0 && n >= 1 && n <= VDISC_MAX_TRACKS) {
                cur_session = n;
                continue;
            }

            /*
             * CD-TEXT BEHIND A `REM', which some writers use for the
             * DISC-LEVEL fields:
             *
             *     REM TITLE "The Ultimate CD-Text Showcase"
             *     REM PERFORMER "The AI Symphony Orchestra"
             *
             * The bare form (TITLE at the top of the file) is the one
             * cdrdao and cdrwin write and is handled below; this is
             * the same data wearing a comment so that a reader which
             * does not know CD-TEXT skips it instead of failing.
             * Both spellings appear in the wild and a sheet may use
             * either - the user supplied one of each, 2026-09-21.
             *
             * ONLY AT THE DISC LEVEL. Inside a TRACK the bare form is
             * what every writer uses, and a `REM TITLE' there would
             * be ambiguous about which it meant.
             */
            if (sscanf(p, "%*s %63s", w) == 1 &&
                cue_cdtext(&img->text, -1, w, strstr(p, w)))
                continue;

            continue;
        }

        /*
         * CD-TEXT. `cur' is -1 until the first TRACK, which is
         * exactly the disc/track distinction the format uses - see
         * cue_cdtext().
         */
        if (cue_cdtext(&img->text,
                       (cur < 0) ? -1 : img->track[cur].num, kw, p))
            continue;

        /* CATALOG, ISRC, REM (other)... ignored */
    }
    fclose(fp);

    if (img->n_tracks == 0) {
        fprintf(stderr, "vdiscd: %s: no tracks found\n", path);
        return -1;
    }
    for (i = 0; i < img->n_tracks; i++) {
        if (ct[i].idx1 < 0) {
            fprintf(stderr, "vdiscd: %s: track %d has no INDEX 01\n",
                    path, img->track[i].num);
            return -1;
        }
        if (ct[i].idx0 >= 0 && ct[i].idx0 > ct[i].idx1) {
            fprintf(stderr, "vdiscd: %s: track %d has INDEX 00 after "
                            "INDEX 01\n", path, img->track[i].num);
            return -1;
        }
    }
    for (k = 0; k < img->n_files; k++) {
        if (img->file[k].secsize == 0) {
            fprintf(stderr, "vdiscd: %s: %s has no TRACK\n",
                    path, img->file[k].path);
            return -1;
        }
    }

    strncpy(img->desc, path, sizeof img->desc - 1);
    img->desc[sizeof img->desc - 1] = '\0';

    /* AND A SIDECAR, if one is beside the sheet. A karaoke `.cdg' is
     * commonly distributed next to a CUE rather than embedded. */
    vdisc_image_find_sub(img, path);

    /*
     * LAYOUT. Walk the files in order and the tracks within each; the
     * disc LBA runs forward as we go. A track's region in its file
     * begins at its first index (INDEX 00 if it has one, else 01) and
     * runs to the next track's first index or the file's end. Before a
     * track's region come the previous track's POSTGAP and this
     * track's PREGAP, as VIRTUAL sectors with no bytes behind them.
     * The TOC start of the track is the disc LBA of its INDEX 01.
     *
     * Every position is checked against the file it indexes, because
     * a .cue is text a user may have edited and the daemon is the
     * untrusted side of the wire.
     */
    disc = 0;
    i = 0;
    for (k = 0; k < img->n_files; k++) {
        int rel = 0;            /* how far into file k we have laid out */
        int pending_post = 0;

        while (i < img->n_tracks && ct[i].file == k) {
            int start = ct[i].idx0 >= 0 ? ct[i].idx0 : ct[i].idx1;

            if (start < rel || ct[i].idx1 >= img->file[k].n_sectors) {
                fprintf(stderr, "vdiscd: %s: track %d's INDEX lies outside "
                                "its file or overlaps the previous track\n",
                        path, img->track[i].num);
                return -1;
            }
            /* the tail of the previous track, then the gaps */
            if (vdisc_image_add_extent(img, k, disc, rel, start - rel) < 0)
                return -1;
            disc += start - rel;
            rel = start;
            if (vdisc_image_add_extent(img, -1, disc, 0, pending_post) < 0)
                return -1;
            disc += pending_post;
            if (vdisc_image_add_extent(img, -1, disc, 0, ct[i].pregap) < 0)
                return -1;
            disc += ct[i].pregap;

            img->track[i].start_lba = disc + (ct[i].idx1 - start);
            pending_post = ct[i].postgap;
            i++;
        }
        if (i < img->n_tracks && ct[i].file < k) {
            fprintf(stderr, "vdiscd: %s: tracks are not in FILE order\n",
                    path);
            return -1;
        }
        /* the rest of the file, then the last track's POSTGAP */
        if (vdisc_image_add_extent(img, k, disc, rel,
                                   img->file[k].n_sectors - rel) < 0)
            return -1;
        disc += img->file[k].n_sectors - rel;
        if (vdisc_image_add_extent(img, -1, disc, 0, pending_post) < 0)
            return -1;
        disc += pending_post;

        if (disc > VDISC_MAX_SECTORS) {
            fprintf(stderr, "vdiscd: %s: the files add up to more than a "
                            "disc (%d sectors)\n", path, disc);
            return -1;
        }
    }

    img->leadout_lba = disc;
    img->first_track = img->track[0].num;
    img->last_track  = img->track[img->n_tracks - 1].num;
    img->n_sessions  = 1;
    for (i = 0; i < img->n_tracks; i++) {
        if (i > 0 && img->track[i].session < img->track[i - 1].session) {
            fprintf(stderr, "vdiscd: %s: REM SESSION goes backwards at "
                            "track %d\n", path, img->track[i].num);
            return -1;
        }
        if (img->track[i].session > img->n_sessions)
            img->n_sessions = img->track[i].session;
    }

    for (i = 0; i < img->n_tracks; i++) {
        int end = (i + 1 < img->n_tracks)
                ? img->track[i + 1].start_lba : img->leadout_lba;
        img->track[i].length = end - img->track[i].start_lba;
        if (img->track[i].length <= 0) {
            fprintf(stderr, "vdiscd: %s: track %d has non-positive length %d\n",
                    path, img->track[i].num, img->track[i].length);
            return -1;
        }
    }

    return 0;
}

static int
cue_read_data(struct vdisc_image *img, int lba, int n, void *buf)
{
    return vdisc_image_read_data(img, lba, n, buf);
}

static int
cue_read_audio(struct vdisc_image *img, int lba, int n, void *buf)
{
    return vdisc_image_read_audio(img, lba, n, buf);
}

static void
cue_close(struct vdisc_image *img)
{
    (void) img;
}

const struct vdisc_backend vdisc_backend_cue = {
    "cue",
    cue_probe,
    cue_open,
    cue_read_data,
    cue_read_audio,
    cue_close
};
