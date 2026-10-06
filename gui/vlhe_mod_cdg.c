/*
 * vlhe_mod_cdg.c - the CD+G viewer.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * READ vlhe_mod_cdg.h FIRST for why this is a MOD_VIEW rather than a
 * form. design/34 has where the subchannel comes from; cdg.c decodes
 * it.
 *
 * THE TRANSPORT IS THE USER'S DESIGN, 2026-09-20: "Button with the
 * standard icons triangle for play square for stop >> << for skip
 * ahead or skip back 30 secs or 10 and guards to start of track or
 * end. -> next track".
 *
 * WHY THE ICONS ARE DRAWN AND NOT LOADED. GTK 1.2 has no stock
 * transport icons - `gtk_pixmap_new' wants an XPM and Corel's theme
 * ships none for this - so each button draws its own triangle or
 * square into a GtkDrawingArea. That is fewer moving parts than
 * shipping nine XPMs, and it scales with the widget.
 *
 * C89, GTK 1.2.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>     /* trace() - see below */
#include <errno.h>      /* on_play() reports which failure - design/45 */
#include <sys/time.h>   /* trace()'s timings - 2026-10-03 */
#include <time.h>

#include "vlhe_buttons.h"
#include "vlhe_mod_cdg.h"
#include "vlhe_picker.h"
#include "cdg.h"
#include "cdtext.h"
/*
 * THE BACKEND, ADDED 2026-09-26 WHEN THE TRANSPORT WAS WIRED.
 *
 * This page was deliberately backend-free while it drew fixture
 * data - it had nothing to ask. It now needs `vlhe_drives()' to
 * find a disc and `vlhe_cd_*()' to work the transport, which is the
 * point at which a viewer becomes a player.
 */
#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_tip.h"
/* THE IMAGE, OPENED READ ONLY AND DECODED HERE - design/34 6e2.
 * The daemon cannot supply this: it does not know where playback
 * has reached, and 96 binary bytes a sector should not cross a
 * process boundary when both parties can read the same file. */
#include "image.h"
/* CD-TEXT FIRST, THEN A CACHED CDDB RECORD - the user's order,
 * 2026-09-26. cddb.h has why we read other players' caches rather
 * than talking to a server. */
#include "cddb.h"
#include "vlhe_journal.h"   /* vlhe_daemon_log_path - see trace() */

/*
 * A TRACE LINE THAT SURVIVES - 2026-09-27, and the reason it exists
 * is a whole wasted run.
 *
 * THE GUI'S stderr GOES NOWHERE. `vlhe_apply.c:3922' redirects each
 * DAEMON's stderr into DAEMON.LOG, but the control centre is started
 * from the desktop menu and its own stderr is discarded. So the four
 * fprintf(stderr) traces added to image_follow() earlier today wrote
 * into the void, TWO RUNS IN A ROW produced no new information, and
 * the same three symptoms were reported each time.
 *
 * THE SAME FILE THE DAEMONS USE, so one capture holds both sides of
 * a disc problem - which is what `readlog.sh' already pulls.
 *
 * OPENED AND CLOSED PER LINE rather than held: this runs a handful
 * of times per disc, never in the poll, and a held descriptor would
 * be one more thing to get wrong on a page that can be detached.
 * Silent if the file cannot be opened - a trace must never be the
 * reason something fails.
 */
/*
 * AND IT FOLLOWS [Tracing] Enabled - 2026-10-03, after a run wrote "no
 * drive with audio" 176 times (tests/logs/2026-10-03-86box-cdrom-
 * drivecount-mixrate). It wrote whatever the setting, unlike every
 * module and daemon. Level 1 is EVENTS - a disc change, a sniff, a
 * failure; level 2 adds the repeating detail (trace2). Off writes
 * nothing.
 */
static void trace_at(int level, const char *fmt, va_list ap);

/*
 * WHAT THE PAGE DOES AFTER A DISC CHANGE, TRACED ONCE EACH - design/54
 * D60, 2026-10-04. A switch from Quake to a CD+G disc left the page
 * half-laid-out for a second or two, and the timed trace showed the
 * switch itself took 5-11 ms and nothing traced in the 2.5 s after it.
 * These mark the steps in that gap, each the first time after a change:
 * the page's layout settling (size_allocate), the picture first drawn
 * by GTK (expose), the first decode (which sniffs the layout) and the
 * first repaint. The gap BEFORE each line is its cost. Level 1, once
 * per change, so they cannot flood.
 */
#define CHG_DECODE   1
#define CHG_REPAINT  2
#define CHG_EXPOSE   4
#define CHG_ALLOC    8
#define CHG_CAUGHT  16
static int g_after_change;

/*
 * AND A FIVE-SECOND WATCH - the second D60 round, 2026-10-04, after the
 * user's account: the picture black and BOTH scrollbars showing, for
 * under a few seconds. The bars are the main window's pane (vlhe_cc.c's
 * g_scroll), raised when this page asks for more height than it has. So
 * for five seconds after "page shown" or "disc change done" EVERY layout
 * of the page is traced - its size and what each child asks for - with
 * the pane's scrollbar state, so the trace names the child that grew and
 * when the bars came and went. Armed on page shown too: an earlier run
 * had a page shown with no disc change and nothing traced after it.
 */
static struct timeval g_watch_until;
static GtkWidget     *g_pane_scroll;    /* vlhe_cc.c's g_scroll, if given */

static void
watch_arm(void)
{
    gettimeofday(&g_watch_until, NULL);
    g_watch_until.tv_sec += 5;
    g_after_change = CHG_DECODE | CHG_REPAINT | CHG_EXPOSE | CHG_ALLOC
                     | CHG_CAUGHT;
}

static int
watching(void)
{
    struct timeval now;

    if (g_watch_until.tv_sec == 0)
        return 0;
    gettimeofday(&now, NULL);
    return now.tv_sec < g_watch_until.tv_sec
           || (now.tv_sec == g_watch_until.tv_sec
               && now.tv_usec < g_watch_until.tv_usec);
}

static void
trace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    trace_at(1, fmt, ap);
    va_end(ap);
}

static void
trace2(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    trace_at(2, fmt, ap);
    va_end(ap);
}

static void
trace_at(int level, const char *fmt, va_list ap)
{
    const char *path;
    FILE       *fp;

    if (vlhe_tracing() < level)
        return;
    path = vlhe_daemon_log_path();
    if (path == NULL)
        return;
    fp = fopen(path, "a");
    if (fp == NULL)
        return;
    /*
     * TIMED - 2026-10-03, the user: "Yes make that change for the
     * tracing", after a disc switch showed a half-laid-out page for a
     * second or two going from Quake to Smash Hits. The wall clock to
     * the millisecond, and the time since the previous trace line: a
     * step's cost is the gap before the line that follows it, so one
     * run says how long the CD+G detection takes on each disc - and on
     * a slower machine than 86Box, which is the case that matters.
     */
    {
        static struct timeval last;
        struct timeval now;
        struct tm *tm;
        long gap;

        gettimeofday(&now, NULL);
        gap = (last.tv_sec == 0) ? 0
            : (now.tv_sec - last.tv_sec) * 1000L
              + (now.tv_usec - last.tv_usec) / 1000L;
        last = now;
        tm = localtime(&now.tv_sec);
        fprintf(fp, "vlhe.gtk/cdg: [%02d:%02d:%02d.%03ld +%ldms] ",
                tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0,
                tm ? tm->tm_sec : 0, (long) now.tv_usec / 1000L, gap);
    }
    vfprintf(fp, fmt, ap);
    fputc('\n', fp);
    fclose(fp);
}

/*
 * 2x by default, which design/32 settled: integer scaling keeps
 * CD+G's 6x12 cells crisp and the pane has room. SETTABLE now,
 * because the choice buys space - at 2x the picture takes 436 of the
 * pane's 522px and leaves one line below it; at 1x it takes 220 and
 * leaves fourteen.
 */
#define ZOOM_MAX    2
#define VIEW_W(z)   (CDG_WIDTH  * (z))
#define VIEW_H(z)   (CDG_HEIGHT * (z))

/* The two skips the user named. Seconds, in CD frames (75/second). */
#define SKIP_SHORT  (10 * 75)
#define SKIP_LONG   (30 * 75)

static void (*g_report)(const char *);

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */

/*
 * THE FIELDS THE INFO ROW CAN SHOW, and the order here is only the
 * order they appear in the `Available' list - the SHOWN list carries
 * the real one, which is the whole point of the widget.
 *
 * `track' and `time' are not CD-TEXT at all; they come from the TOC
 * and are always available, which is why a disc with no CD-TEXT still
 * has something to put in the row.
 */
static const char *const g_field_name[] = {
    "title", "performer", "songwriter", "composer",
    "arranger", "message", "track", "time"
};
static const char *const g_field_label[] = {
    STR_CDG_FIELD_TITLE, STR_CDG_FIELD_PERFORMER, STR_CDG_FIELD_SONGWRITER, STR_CDG_FIELD_COMPOSER,
    STR_CDG_FIELD_ARRANGER, STR_CDG_FIELD_MESSAGE, STR_CDG_FIELD_TRACK_NUMBER, STR_CDG_FIELD_TIME
};
#define N_FIELDS  8

static int g_zoom = 2;

/*
 * MASK THE OUTER TILE WITH THE BORDER COLOUR.
 *
 * CD+G's field is 300x216 and a player shows the inside of it: the
 * outer tile is the BORDER, and the margin behind it holds content
 * waiting to be scrolled in. Drawing the whole field shows that
 * staging area, which is not what any reference player does.
 *
 * MEASURED ON `Jimi Hendrix - Smash Hits'. Its scroll feeds new
 * material in through COLUMN 0 - 3388 tiles against 25 for column
 * 1 - so with no mask the left edge shows tiles a moment before
 * they are meant to appear. The user saw exactly that, as a thin
 * purple sliver at the left and a band across the top.
 *
 * AND IT IS NOT THE SCROLL. `tests/cdgcmp/scrollcmp.c' feeds that
 * disc's own scroll commands to our algorithm and to cdgdeck's and
 * diffs the buffer after each: 124 scrolls, 0 differences. The
 * mask is the whole remaining difference.
 *
 * cdgdeck does the same thing and makes it optional
 * (`cdgrenderer.cpp', maskBorder); so do we.
 */
#define CDG_MASK_W  CDG_TILE_W          /* 6px each side   */
#define CDG_MASK_H  CDG_TILE_H          /* 12px top/bottom */

static int g_border_mask = 1;
/* CLEAR THE PICTURE WHEN THE DISC GOES - on by default, the user's
 * call 2026-10-01 after an eject left the last frame and the old
 * disc's line under a track menu saying "(no disc)". Off keeps the
 * last frame until a new disc paints over it, which is what the
 * viewer did before and what a player's screen does. The information
 * line clears either way: it names a disc that is not there. */
static int g_clear_on_eject = 1;
/* DETACHED, THE SECOND ROW - the user, 2026-10-01: "Add a Compact when
 * detached option. That moves the attach and drop down below the
 * player controls and reducing the width of the cd+g detached player
 * but making it one row taller." */
static int g_compact_detached = 0;
static int g_track_format;              /* 0 title, 1 artist - title  */
static char g_info_fields[128] = "performer,title";

/*
 * WHAT IS IN THE DRIVE, as far as this page knows.
 *
 * MODULE LEVEL BECAUSE THE OPTIONS DIALOG NEEDS IT. Changing the
 * track-list format has to rebuild the dropdown, and a dialog that
 * cannot reach the disc can only store the setting and leave the
 * screen stale - which is exactly what happened: the user chose
 * artist-and-title, pressed OK, and nothing changed.
 *
 * The demo content in cdg_build() fills these the same way a real
 * disc will.
 */
/*
 * THE TOC WE HOLD, KsCD's ARRANGEMENT - `cdrom.c:431' computes a
 * track-relative position from a table it read once, and asks no
 * daemon. Refreshed when the drive we act on changes, which is the
 * only time a disc's layout can have.
 */
#define CDG_MAX_TOC 99                  /* the CD maximum            */
static struct vlhe_cd_track g_toc[CDG_MAX_TOC];
static int                  g_ntoc;
static int                  g_toc_drive = -1;
/* THE DISC BEHIND THAT TOC - so a swap into the same drive is seen.
 * See follow_disc(); the index alone cannot tell one disc from the
 * next in a one-drive machine. */
static char                 g_toc_image[VLHE_PATH_MAX];

/*
 * THE ATTACHED IMAGE, OURS TO READ - design/34 6e2.
 *
 * OPENED READ ONLY, ALONGSIDE THE DAEMON AND THE KERNEL, and that
 * is safe rather than merely tolerated: `image.c:137' is
 * `fopen(path, "rb")' and nothing in `image.c' or any `vdiscd' file
 * takes a lock - no flock, no lockf, no F_SETLK. The kernel module
 * already reads the same file on every block request, so a third
 * reader is not new in kind.
 *
 * `g_sub_lba' IS WHERE THE DECODE HAS REACHED, which is not where
 * playback is: the screen at sector N depends on every packet
 * before it, so we walk forward and remember how far. -1 means
 * nothing decoded yet.
 */
static struct vdisc_image g_img;
static int                g_img_open;
static int                g_sub_lba = -1;
static int                g_sub_layout = CDG_SUB_PW96_INTERLEAVED;

/*
 * DOES THIS DISC CARRY GRAPHICS? Decided at attach, so an ordinary
 * audio CD costs nothing at all: the poll returns before its ioctl
 * and no sector is ever read or decoded.
 *
 * THE USER'S ASK, 2026-09-26: *"for non cd+g can we disable the
 * display or keep it black. Make it more performant if just playing
 * audio"*. Most discs have no CD+G, so this is the common case
 * rather than an optimisation for a rare one.
 */
/*
 * HOW FAR TO WALK LOOKING FOR SUBCHANNEL CONTENT, in 32-sector
 * spans. ~14 seconds of audio, which is far more lead-in silence
 * than any disc has.
 *
 * USED BY TWO PLACES AND THAT IS THE POINT: the graphics probe in
 * image_follow() and sniff_layout(). Both once looked only at the
 * track start, where every disc is quiet, and both drew the wrong
 * conclusion from it.
 */
#define CDG_SNIFF_SPANS 24

static int                g_has_graphics;

static struct cdtext g_disc;
static int           g_disc_tracks;
static int           g_disc_cur = 1;     /* selected track            */

static GtkWidget       *g_area;         /* the picture                 */

/*
 * WHAT WE BLIT FROM: A GdkImage OF READY-TO-DISPLAY PIXELS.
 *
 * THIS WAS A GdkPixmap AND A FULL-SCREEN RECTANGLE LOOP until
 * 2026-09-29, and the old shape is worth stating because the reason
 * it had to go is measurable. repaint() walked all 216 rows, built
 * horizontal runs of equal palette index, and issued one
 * gdk_draw_rectangle() per run - THOUSANDS of separate X requests
 * per repaint, four times a second, whatever had actually changed.
 * It also called gdk_color_alloc() inside that loop, for a palette
 * of sixteen colours, thousands of times a second.
 *
 * Measured on 86Box at 2x zoom with `top': XF86_Mach64 10-20% CPU
 * against vlhe.gtk's 5-10%, growing with zoom. The X SERVER was
 * costing two to four times what we were - it was blit-bound, and
 * the cost was REQUEST COUNT.
 *
 * So the picture now lives in one GdkImage at view size, holding
 * pixels in the display's own format. We convert only the tiles the
 * decoder marked (cdg_row_span), and an expose is a straight
 * gdk_draw_image() with NO conversion at all - where the old path
 * re-ran the whole rectangle loop for every expose.
 *
 * WHY NOT gdk_draw_indexed_image(). It is the obvious call, it is in
 * this target's GTK (usr/include/gdk/gdkrgb.h), and it would be one
 * request. But GdkRGB only specialises the indexed path at 8bpp
 * (gdkrgb.c:2806); at 15/16/24/32 it takes
 * gdk_rgb_convert_indexed_generic, which expands indices into a
 * 24-bit staging buffer and THEN runs the depth converter - two
 * passes over every pixel. Our machines run 16, 24 or 32. Writing
 * the pixels ourselves is one pass, and folds the 2x zoom into the
 * same loop.
 */
static GdkImage        *g_view;         /* view-sized, display format  */
static GdkGC           *g_gc;           /* for gdk_draw_image()        */
static unsigned long    g_pix[CDG_COLOURS];   /* CLUT -> screen pixel  */
static unsigned short   g_pix_clut[CDG_COLOURS]; /* what g_pix is for  */
static int              g_pix_valid;

static struct cdg_screen g_screen;
static int              g_active;
static GtkWidget       *g_tracks;   /* the track dropdown             */
/*
 * SET WHILE WE ARE DRIVING THE DROPDOWN OURSELVES.
 *
 * `tracks_select()' moves the selection to follow the PLAYING
 * track, and GTK cannot tell that from a user's pick - both fire
 * "changed". Since a user's pick now STARTS PLAYBACK (KsCD's
 * behaviour), an unguarded update would restart the current track
 * on every poll, four times a second.
 */
static int              g_selecting;

/*
 * IS THE CD+G PAGE THE SELECTED ONE? Kept apart from `g_active',
 * which is DERIVED from this OR a detached window being up. The
 * shell only knows about the sidebar; detach and attach happen in
 * here, and both have to re-derive.
 */
static int              g_page_selected;
static GtkWidget       *g_info;     /* the information line           */
/*
 * THE TIME READOUT - the user's ask, 2026-09-28: *"We should display
 * the track time somewhere updated every second to show something is
 * playing if audio is not heard."*
 *
 * THE POINT IS THE SECOND HALF. On a machine where the card is not
 * draining - the emulated ESS Solo-1, `design/44' - the daemons
 * report healthy, the ioctls all return 0 and NOTHING on screen
 * moves. A running clock separates "the disc is turning and you
 * cannot hear it" from "nothing is happening at all", which is the
 * distinction that cost most of 2026-09-28 to make from logs.
 *
 * IT FOLLOWS THE POSITION, NOT A TIMER OF ITS OWN. The value comes
 * from the same `vlhe_cd_position()' the poll already calls, so it
 * cannot disagree with the picture or the dropdown - and if the
 * position stops advancing the clock stops with it, which is
 * exactly the symptom worth seeing.
 */
static GtkWidget       *g_time;     /* m:ss elapsed in the track      */
/* Rate limit for the "no decode" line below - file scope so the
 * decode path can reset it. */
static int              g_nodecode_said;
static GtkWidget       *g_float;    /* the detached window, or NULL   */
static GtkWidget       *g_vbox;     /* what moves between the two     */
static GtkWidget       *g_slot;     /* where it lives when attached   */
static GtkWidget       *g_detach_btn;
static GtkWidget       *g_hide_btn;     /* only useful when detached */
static GtkWidget       *g_row1;         /* transport, Options, Hide, Detach */
static GtkWidget       *g_row2;         /* the compact second row, or hidden */
static GtkWidget       *g_picture;      /* the alignment holding it  */
static int              g_hidden;       /* picture hidden: player only */
/*
 * WHERE THE PICTURE SITS, AS A NUMPAD CELL - the user's design,
 * 2026-09-20:
 *
 *      7  8  9      top-left     top-centre     top-right
 *      4  5  6      centre-left  CENTRE         centre-right
 *      1  2  3      bottom-left  bottom-centre  bottom-right
 *
 * ONE VALUE SERVES BOTH STATES. Detached, only the COLUMN is read -
 * 7/4/1 left, 8/5/2 centre, 9/6/3 right - because the float window
 * fits the picture vertically and there is nothing to move within.
 * That is the user's simplification and it removes a second setting.
 *
 * THE CONTROLS DO NOT MOVE. They stay pinned to the bottom of the
 * pane, above the shell's own button row, and the picture anchors in
 * whatever rectangle is left above them. Two regions, not one block.
 */
static int              g_anchor = 5;

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

/*
 * CD+G'S 4-BIT-PER-CHANNEL COLOUR TO GDK'S 16-BIT.
 *
 * 0x0RGB with four bits each, so a channel scales by 0x1111 rather
 * than by 0x1000 - the difference is whether full-scale reaches
 * 0xffff or stops at 0xf000, and the second gives a picture that is
 * subtly dark everywhere.
 */
static void
cdg_colour(unsigned short rgb, GdkColor *out)
{
    out->red   = (guint16)(((rgb >> 8) & 0x0f) * 0x1111);
    out->green = (guint16)(((rgb >> 4) & 0x0f) * 0x1111);
    out->blue  = (guint16)(( rgb       & 0x0f) * 0x1111);
    out->pixel = 0;
}

/*
 * THE SIXTEEN SCREEN PIXELS, BUILT ONCE PER PALETTE CHANGE.
 *
 * This is the allocation the old repaint() did per RUN per ROW per
 * CALL. A CD+G disc has sixteen colours; it needs doing when they
 * change and at no other time.
 *
 * Returns 1 if the table moved, so the caller knows every tile has
 * to be reconverted.
 */
static int
palette_sync(void)
{
    GdkColormap *cm;
    int i, changed = 0;

    if (g_area == NULL || !GTK_WIDGET_REALIZED(g_area))
        return 0;

    if (g_pix_valid &&
        memcmp(g_pix_clut, g_screen.clut, sizeof g_pix_clut) == 0)
        return 0;

    cm = gtk_widget_get_colormap(g_area);
    for (i = 0; i < CDG_COLOURS; i++) {
        GdkColor c;

        cdg_colour(g_screen.clut[i], &c);
        gdk_color_alloc(cm, &c);
        g_pix[i] = (unsigned long) c.pixel;
    }
    memcpy(g_pix_clut, g_screen.clut, sizeof g_pix_clut);
    g_pix_valid = 1;
    changed = 1;
    return changed;
}

/*
 * ONE SOURCE PIXEL AS A SCREEN PIXEL, honouring the field's edge.
 *
 * `src' is NULL when the displaced row lies outside the field.
 * Outside in either direction is the BORDER COLOUR, which is the
 * rule cdg_rgb_at() has always followed - and it is why
 * BORDER_PRESET stores an index and writes no pixels (cdg.h has
 * the trap, and this disc is the one that proves it).
 */
static unsigned long
src_pixel(const unsigned char *src, int sx)
{
    if (src == NULL || sx < 0 || sx >= CDG_WIDTH)
        return g_pix[g_screen.border & 0x0f];
    return g_pix[src[sx] & 0x0f];
}

/*
 * IS THIS SCREEN PIXEL UNDER THE BORDER MASK? Screen coordinates,
 * before zoom - the ring is one tile wide on every side.
 */
static int
in_mask(int x, int y)
{
    if (!g_border_mask)
        return 0;
    return x < CDG_MASK_W || x >= CDG_WIDTH  - CDG_MASK_W
        || y < CDG_MASK_H || y >= CDG_HEIGHT - CDG_MASK_H;
}

/*
 * CONVERT A TILE-ROW SPAN INTO THE IMAGE, doing the zoom in the same
 * pass.
 *
 * `row' is a tile row; col0/col1 are a half-open tile range. Works in
 * CD+G pixels and scales by g_zoom on the way out, so the doubled
 * rows are written from the row we just built rather than converted
 * twice.
 */
static void
convert_span(int row, int col0, int col1)
{
    int z = g_zoom;
    int x0 = col0 * CDG_TILE_W;
    int x1 = col1 * CDG_TILE_W;
    int y0 = row  * CDG_TILE_H;
    int y1 = y0 + CDG_TILE_H;
    /*
     * THE FINE SCROLL OFFSET, AND IT WAS IGNORED UNTIL 2026-09-29.
     *
     * A CD+G scroll moves the picture in two steps: `h_offset'
     * slides it one pixel at a time (0-5 across, 0-11 down), and
     * when a whole tile has been covered a coarse SCROLL_COPY
     * shifts by CDG_TILE_W and the offset resets to 0. THE TWO ARE
     * ONE MOVEMENT, and applying the coarse half alone is what the
     * user saw on the Hendrix disc - "stuff on the right is drawn
     * on the left" as a wrapped column arrived up to five pixels
     * out of place, jumping a whole tile at a time instead of
     * sliding.
     *
     * 577 of that disc's 684 scrolls change only the offset, so
     * this is the usual case rather than a corner of it.
     *
     * THE WRAP ITSELF IS CORRECT AND IS NOT THIS BUG: the disc asks
     * for SCROLL_COPY, which wraps the right edge round to the left
     * by design. What was wrong was where that content landed.
     */
    int hoff = g_screen.h_offset;
    int voff = g_screen.v_offset;
    int y;

    if (g_view == NULL)
        return;
    if (x1 > CDG_WIDTH)  x1 = CDG_WIDTH;
    if (y1 > CDG_HEIGHT) y1 = CDG_HEIGHT;

    /*
     * THE DEPTH IS DECIDED ONCE PER SPAN, NOT ONCE PER PIXEL.
     *
     * THIS USED gdk_image_put_pixel() AND THAT WAS THE WHOLE COST.
     * Measured on 86Box at 2x: the change to a GdkImage dropped the
     * X server from 10-20% to about 5%, and pushed vlhe.gtk from
     * 5-10% to 10-30% - the work moved rather than went away, and
     * the total was no better.
     *
     * put_pixel() is a function call that switches on the image's
     * bit depth EVERY CALL, and the row doubling then called it
     * again per pixel with a get_pixel() beside it - roughly four
     * dispatched calls per source pixel. Hoisting the switch out to
     * here leaves a plain store through a typed row pointer, and the
     * doubled row becomes one memcpy.
     *
     * `bpl' is bytes per LINE and need not equal width * bpp - X
     * pads scanlines - so rows are stepped in bytes and only the
     * pixels within a row are typed.
     */
    for (y = y0; y < y1; y++) {
        /*
         * THE SOURCE ROW IS DISPLACED BY THE OFFSET, AND DOES NOT
         * WRAP - corrected 2026-09-29, having been written wrapping
         * hours earlier.
         *
         * The reasoning for wrapping was that the coarse step this
         * interpolates is SCROLL_COPY, which wraps, so the fine
         * step should agree. THAT CONFLATES TWO DIFFERENT THINGS.
         * SCROLL_COPY moves PIXELS WITHIN THE BUFFER and wraps them
         * as it goes. The fine offset moves the VIEWPORT over a
         * field that is deliberately larger than the visible area,
         * so what lies past the edge is the MARGIN - content
         * waiting to be scrolled in - and past that, the border.
         *
         * Wrapping instead pulled the far edge round: the user saw
         * part of the "A" in "SMASH" drawn at the left beside the
         * "H" of "HENDRIX", and top content appearing at the
         * bottom. Neither reference player does that.
         *
         * `cdg_rgb_at()' has always had the right rule and this
         * now matches it: outside the field is the border colour.
         * CDG_BORDER_IDX below is that colour's slot.
         */
        int sy = y + voff;
        const unsigned char *src;
        unsigned char *row = (unsigned char *) g_view->mem
                           + (size_t)(y * z) * g_view->bpl;
        int span = (x1 - x0) * z;
        int direct = 1;         /* cleared by the fallback case */
        int x;

        /* Past the bottom of the field there is no row to read, so
         * the whole span is border. */
        src = (sy >= 0 && sy < CDG_HEIGHT)
            ? g_screen.pixel + sy * CDG_WIDTH : NULL;

        switch (g_view->bpp) {
        case 2: {
            guint16 *d = (guint16 *) (row + (size_t)(x0 * z) * 2);

            for (x = x0; x < x1; x++) {
                guint16 p = (guint16) (in_mask(x, y)
                    ? g_pix[g_screen.border & 0x0f]
                    : src_pixel(src, x + hoff));
                int k;

                for (k = 0; k < z; k++)
                    *d++ = p;
            }
            break;
        }
        case 4: {
            guint32 *d = (guint32 *) (row + (size_t)(x0 * z) * 4);

            for (x = x0; x < x1; x++) {
                guint32 p = (guint32) (in_mask(x, y)
                    ? g_pix[g_screen.border & 0x0f]
                    : src_pixel(src, x + hoff));
                int k;

                for (k = 0; k < z; k++)
                    *d++ = p;
            }
            break;
        }
        case 1: {
            unsigned char *d = row + (size_t)(x0 * z);

            for (x = x0; x < x1; x++) {
                unsigned char p = (unsigned char) (in_mask(x, y)
                    ? g_pix[g_screen.border & 0x0f]
                    : src_pixel(src, x + hoff));
                int k;

                for (k = 0; k < z; k++)
                    *d++ = p;
            }
            break;
        }
        default:
            /*
             * 24bpp PACKED, or anything unexpected. Three bytes per
             * pixel has no integer type, and the byte order is the
             * image's rather than the host's - so this one case uses
             * GDK's accessor and pays for it. No machine here runs
             * it: the guest is depth 24 at 32 BITS per pixel, which
             * is case 4 above.
             *
             * `direct' GOVERNS BOTH HALVES so they cannot drift: a
             * depth handled by a typed store here is a depth the
             * row copy below may memcpy, and any other depth uses
             * the accessor in both places.
             */
            direct = 0;
            for (x = x0; x < x1; x++) {
                unsigned long p = in_mask(x, y)
                    ? g_pix[g_screen.border & 0x0f]
                    : src_pixel(src, x + hoff);
                int k;

                for (k = 0; k < z; k++)
                    gdk_image_put_pixel(g_view, x * z + k, y * z, p);
            }
            break;
        }

        /*
         * THE OTHER ROWS OF A ZOOMED PIXEL ARE A COPY of the one we
         * just built - the pixels are already in the display's
         * format, so there is nothing to convert twice.
         */
        if (z > 1 && direct) {
            size_t off = (size_t)(x0 * z) * g_view->bpp;
            size_t len = (size_t) span * g_view->bpp;
            int k;

            for (k = 1; k < z; k++)
                memcpy(row + (size_t) k * g_view->bpl + off,
                       row + off, len);
        } else if (z > 1) {
            int k, vx;

            for (k = 1; k < z; k++)
                for (vx = x0 * z; vx < x1 * z; vx++)
                    gdk_image_put_pixel(g_view, vx, y * z + k,
                                        gdk_image_get_pixel(g_view,
                                                            vx, y * z));
        }
    }
}

/* Push a view-pixel rectangle to the window: one X request. */
static void
show_rect(int vx, int vy, int vw, int vh)
{
    int mw = VIEW_W(g_zoom);
    int mh = VIEW_H(g_zoom);

    if (g_view == NULL || g_gc == NULL ||
        g_area == NULL || !GTK_WIDGET_DRAWABLE(g_area))
        return;

    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > mw) vw = mw - vx;
    if (vy + vh > mh) vh = mh - vy;
    if (vw <= 0 || vh <= 0)
        return;

    gdk_draw_image(g_area->window, g_gc, g_view,
                   vx, vy, vx, vy, vw, vh);
}

/* Cell to xalign/yalign. Column 0.0/0.5/1.0, row likewise, and the
 * numpad runs bottom-up so row 7-8-9 is the TOP. */
static void
anchor_align(int cell, gfloat *xa, gfloat *ya)
{
    int col = (cell - 1) % 3;           /* 0 left, 1 centre, 2 right  */
    int row = (cell - 1) / 3;           /* 0 bottom, 1 middle, 2 top  */

    *xa = (gfloat)(col * 0.5);
    *ya = (gfloat)(1.0 - row * 0.5);    /* invert: row 2 is the top   */
}

static void repaint(void);
static void cdg_fit_float(void);

/*
 * PUT THE PICTURE WHERE THE ANCHOR SAYS.
 *
 * DETACHED READS THE COLUMN ONLY. The float window is sized to its
 * contents, so there is no vertical slack to move within - and
 * horizontally there is only slack if the user widened it by hand.
 */
static void
cdg_apply_anchor(void)
{
    gfloat xa, ya;

    if (g_picture == NULL)
        return;

    anchor_align(g_anchor, &xa, &ya);
    if (g_float != NULL)
        ya = 0.0;               /* nothing to move within vertically */

    gtk_alignment_set(GTK_ALIGNMENT(g_picture), xa, ya, 0.0, 0.0);
}
static void on_track_picked(gint row, const gchar *text,
                            gpointer d);
static void tracks_load(int n_tracks, const struct cdtext *ct);
/* Point the dropdown at a track, as KsCD does - defined below. */
static void tracks_select(int track);
/* Defined below with the Hide button; image_follow() collapses the
 * picture through it when a disc carries no graphics. */
static void cdg_set_hidden(int hidden);
static void info_load(int track, const struct cdtext *ct);

/* Drop the image and its GC - on a zoom change, and at teardown. */
static void
image_free(void)
{
    if (g_view != NULL) {
        gdk_image_destroy(g_view);
        g_view = NULL;
    }
    if (g_gc != NULL) {
        gdk_gc_unref(g_gc);
        g_gc = NULL;
    }
}

/*
 * The image needs a realised widget for its visual, so it is made
 * here rather than in cdg_build().
 *
 * GDK_IMAGE_FASTEST gives shared memory where the server offers it -
 * MIT-SHM, confirmed present on this target by xdpyinfo - and falls
 * back to a normal image where it does not. Either way the pixels
 * are in the display's own format and an expose needs no conversion.
 */
static void
image_make(GtkWidget *w)
{
    if (g_view != NULL || w == NULL || w->window == NULL)
        return;

    g_view = gdk_image_new(GDK_IMAGE_FASTEST,
                          gtk_widget_get_visual(w),
                          VIEW_W(g_zoom), VIEW_H(g_zoom));
    if (g_view == NULL)
        return;
    if (g_gc == NULL)
        g_gc = gdk_gc_new(w->window);

    /* A NEW IMAGE HOLDS NOTHING, so everything must be converted
     * into it before the next expose reads it. */
    g_pix_valid = 0;
    cdg_mark_all(&g_screen);
}

static void
on_realize(GtkWidget *w, gpointer data)
{
    (void) data;

    image_make(w);
    repaint();
}

/*
 * EXPOSE IS NOW A STRAIGHT COPY. The old handler blitted from a
 * pixmap, which was already cheap; the expensive part was that the
 * pixmap had to be rebuilt by the full rectangle loop. Here the
 * image is always current, so an expose converts nothing.
 */
static gint
on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void) w; (void) data;

    if (g_view == NULL)
        return FALSE;

    if (g_after_change & CHG_EXPOSE) {
        g_after_change &= ~CHG_EXPOSE;
        trace("first picture expose after the change: %dx%d at %d,%d",
              ev->area.width, ev->area.height, ev->area.x, ev->area.y);
    }
    show_rect(ev->area.x, ev->area.y, ev->area.width, ev->area.height);
    return FALSE;
}

/*
 * REPAINT THE BACKING STORE FROM THE DECODED SCREEN.
 *
 * ROW BY ROW, NOT PIXEL BY PIXEL. A naive loop would issue 129,600
 * one-pixel draws per frame; this one walks each row and emits a
 * filled rectangle per RUN of equal pixels, which on CD+G text is a
 * handful per row because the picture is tiles of flat colour.
 */
/* THE PAGE'S LAYOUT SETTLING after a change - D60, see g_after_change. */
/* ONE WIDGET'S LINE: its type, what it ASKS for and what it GOT. */
static void
trace_child(const char *indent, GtkWidget *c)
{
    trace("%s%s%s asks %dx%d, got %dx%d", indent,
          gtk_type_name(GTK_OBJECT_TYPE(c)),
          GTK_WIDGET_VISIBLE(c) ? "" : " (hidden)",
          c->requisition.width, c->requisition.height,
          c->allocation.width, c->allocation.height);
}

static void
trace_layout(GtkAllocation *a)
{
    GList *l, *k;

    trace("page laid out: %dx%d (asks %dx%d)", a->width, a->height,
          g_slot->requisition.width, g_slot->requisition.height);
    /* TWO LEVELS DOWN - g_slot holds the page box, the page box holds
     * the rows; the row that asks too much is the one that grew. */
    for (l = gtk_container_children(GTK_CONTAINER(g_slot)); l != NULL;
         l = l->next) {
        GtkWidget *c = GTK_WIDGET(l->data);

        trace_child("  ", c);
        if (GTK_IS_CONTAINER(c))
            for (k = gtk_container_children(GTK_CONTAINER(c)); k != NULL;
                 k = k->next)
                trace_child("    ", GTK_WIDGET(k->data));
    }
    if (g_pane_scroll != NULL) {
        GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(g_pane_scroll);

        trace("  pane %dx%d: vertical bar %s, horizontal bar %s",
              g_pane_scroll->allocation.width,
              g_pane_scroll->allocation.height,
              sw->vscrollbar != NULL && GTK_WIDGET_VISIBLE(sw->vscrollbar)
                  ? "SHOWN" : "hidden",
              sw->hscrollbar != NULL && GTK_WIDGET_VISIBLE(sw->hscrollbar)
                  ? "SHOWN" : "hidden");
    }
}

static void
on_slot_allocate(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    (void) w; (void) data;
    if (g_after_change & CHG_ALLOC) {
        g_after_change &= ~CHG_ALLOC;
        trace("page laid out after the change: %dx%d", a->width, a->height);
    }
    if (watching())
        trace_layout(a);
}

/* THE PANE'S OWN LAYOUT - the bars can come or go without this page
 * being reallocated, so the pane is watched too, for the same window. */
static void
on_pane_allocate(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(w);

    (void) data;
    if (!g_active || !watching())
        return;
    trace("pane laid out: %dx%d - vertical bar %s, horizontal bar %s",
          a->width, a->height,
          sw->vscrollbar != NULL && GTK_WIDGET_VISIBLE(sw->vscrollbar)
              ? "SHOWN" : "hidden",
          sw->hscrollbar != NULL && GTK_WIDGET_VISIBLE(sw->hscrollbar)
              ? "SHOWN" : "hidden");
}

void
cdg_set_pane_scroll(GtkWidget *sw)
{
    g_pane_scroll = sw;
    if (sw != NULL)
        gtk_signal_connect(GTK_OBJECT(sw), "size_allocate",
                           GTK_SIGNAL_FUNC(on_pane_allocate), NULL);
}

static void
repaint(void)
{
    int z = g_zoom;
    int row;

    if (g_view == NULL || g_area == NULL || g_area->window == NULL)
        return;

    /*
     * A PALETTE CHANGE INVALIDATES EVERY CONVERTED PIXEL, because the
     * image holds resolved colours where cdg_screen holds indices.
     * load_clut() already marks every tile, so the walk below
     * reconverts the screen - this only has to rebuild the table
     * before it runs.
     */
    (void) palette_sync();

    for (row = 0; row < CDG_ROWS; row++) {
        int col0, col1;

        if (!cdg_row_span(&g_screen, row, &col0, &col1))
            continue;           /* nothing waiting in this row */

        convert_span(row, col0, col1);
        show_rect(col0 * CDG_TILE_W * z, row * CDG_TILE_H * z,
                  (col1 - col0) * CDG_TILE_W * z, CDG_TILE_H * z);
    }

    cdg_take_tiles(&g_screen);
}

/* ------------------------------------------------------------------ */
/* The transport icons, drawn rather than loaded                      */
/* ------------------------------------------------------------------ */

/* The play button's drawing area - transport_set() reskins it as a
 * pause bar while playing. Declared here because icon_button()
 * below captures it. */
static GtkWidget *g_play_icon;

/*
 * WHAT THE TRANSPORT IS DOING. Read by time_show() as well as by
 * the buttons, because a paused drive reports `playing' as 0 and
 * the clock must still show its position (KsCD, kscd.cpp:1327).
 */
enum { TRANSPORT_STOPPED, TRANSPORT_PLAYING, TRANSPORT_PAUSED };

static int g_transport = TRANSPORT_STOPPED;

/* Defined with the transport buttons; image_follow() clears the
 * state when the disc changes, and that is above them. */
static void transport_set(int state);
static int  follow_disc(void);

enum {
    ICON_PLAY,      /* triangle                                        */
    ICON_PAUSE,     /* two bars - the play button becomes this         */
    ICON_STOP,      /* square                                          */
    ICON_RW,        /* << two triangles, pointing left                 */
    ICON_FF,        /* >>                                              */
    ICON_PREV,      /* |< a bar and a triangle - start of track        */
    ICON_NEXT       /* >| the mirror - next track                      */
};

static gint
on_icon_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    /*
     * THE GLYPH IS USUALLY FIXED AND THE PLAY BUTTON'S IS NOT.
     * Every other icon is whatever was passed at construction; the
     * play button carries an "icon" object datum that
     * transport_set() rewrites, so it can show a pause bar while
     * playing. Absent datum means the fixed value, which is every
     * other button.
     */
    gpointer dyn = gtk_object_get_data(GTK_OBJECT(w), "icon");
    int which = (dyn != NULL) ? GPOINTER_TO_INT(dyn) - 1
                              : GPOINTER_TO_INT(data);
    GdkGC *gc;
    GdkPoint tri[3];
    int cx, cy, h;

    (void) ev;

    if (w->window == NULL)
        return FALSE;

    gc = w->style->fg_gc[GTK_WIDGET_STATE(w)];
    cx = w->allocation.width  / 2;
    cy = w->allocation.height / 2;
    h  = 5;                     /* half-height of a glyph, in pixels  */

    switch (which) {
    case ICON_PLAY:
        tri[0].x = cx - 4; tri[0].y = cy - h;
        tri[1].x = cx - 4; tri[1].y = cy + h;
        tri[2].x = cx + 6; tri[2].y = cy;
        gdk_draw_polygon(w->window, gc, TRUE, tri, 3);
        break;

    case ICON_PAUSE:
        gdk_draw_rectangle(w->window, gc, TRUE, cx - 5, cy - 5, 3, 10);
        gdk_draw_rectangle(w->window, gc, TRUE, cx + 2, cy - 5, 3, 10);
        break;

    case ICON_STOP:
        gdk_draw_rectangle(w->window, gc, TRUE,
                           cx - 5, cy - 5, 10, 10);
        break;

    case ICON_FF:
    case ICON_RW: {
        int dir = (which == ICON_FF) ? 1 : -1;
        int k;

        for (k = 0; k < 2; k++) {
            int ox = cx + dir * (k * 7 - 7);

            tri[0].x = ox;           tri[0].y = cy - h;
            tri[1].x = ox;           tri[1].y = cy + h;
            tri[2].x = ox + dir * 6; tri[2].y = cy;
            gdk_draw_polygon(w->window, gc, TRUE, tri, 3);
        }
        break;
    }

    case ICON_PREV:
    case ICON_NEXT: {
        int dir = (which == ICON_NEXT) ? 1 : -1;
        int bar = cx + dir * 6;

        /* THE BAR IS THE "GUARD" the user asked for - the glyph that
         * says this one stops at a boundary rather than scrubbing. */
        gdk_draw_rectangle(w->window, gc, TRUE,
                           bar - 1, cy - h, 2, h * 2);
        tri[0].x = cx - dir * 5; tri[0].y = cy - h;
        tri[1].x = cx - dir * 5; tri[1].y = cy + h;
        tri[2].x = cx + dir * 4; tri[2].y = cy;
        gdk_draw_polygon(w->window, gc, TRUE, tri, 3);
        break;
    }
    default:
        break;
    }

    return FALSE;
}

/* A transport button: a drawn glyph in a button, with a tooltip
 * because a drawn icon carries no label. */
static GtkWidget *
icon_button(int which, const char *tip, GtkSignalFunc cb,
            GtkTooltips *tips)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *area = gtk_drawing_area_new();

    /* THE PLAY BUTTON'S GLYPH CHANGES, so transport_set() needs the
     * drawing area rather than the button. Nothing else does. */
    if (which == ICON_PLAY)
        g_play_icon = area;

    gtk_drawing_area_size(GTK_DRAWING_AREA(area), 22, 18);
    gtk_signal_connect(GTK_OBJECT(area), "expose_event",
                       GTK_SIGNAL_FUNC(on_icon_expose),
                       GINT_TO_POINTER(which));
    gtk_container_add(GTK_CONTAINER(btn), area);
    gtk_widget_show(area);

    if (cb != NULL)
        gtk_signal_connect(GTK_OBJECT(btn), "clicked", cb, NULL);

    gtk_tooltips_set_tip(tips, btn, tip, NULL);
    return btn;
}

/* ------------------------------------------------------------------ */
/* The Options dialog                                                 */
/* ------------------------------------------------------------------ */

/*
 * WHY A DIALOG AND NOT A TAB. A MOD_VIEW page has no notebook and no
 * button row (design/32 section 6) - that is what makes the picture
 * fit - and at 2x it leaves 28px below the transport. There is
 * nowhere to put a settings pane, so it goes behind a button.
 */

static GtkWidget *g_dlg;
static GtkWidget *g_avail;          /* GtkCList of unused fields      */
static GtkWidget *g_shown;          /* GtkCList, IN DISPLAY ORDER     */
static GtkWidget *g_zoom_1, *g_zoom_2;
static GtkWidget *g_mask_btn;
static GtkWidget *g_clear_btn;   /* "Clear the picture" on eject */
static GtkWidget *g_compact_btn; /* "Compact when detached"      */
static void layout_rows(void);   /* defined with detach_set(), below */
static GtkWidget *g_anchor_btn[9];   /* screen order: 7 8 9 / 4 5 6 / 1 2 3 */
static GtkWidget *g_fmt_title, *g_fmt_artist;

/* Find a field by its config name. -1 if it is not one of ours. */
static int
field_index(const char *name)
{
    int i;

    for (i = 0; i < N_FIELDS; i++)
        if (strcmp(g_field_name[i], name) == 0)
            return i;
    return -1;
}

/*
 * FILL THE TWO LISTS FROM THE SETTING.
 *
 * `Shown' takes the order in g_info_fields exactly; `Available' takes
 * everything else in the declaration order. So the widget's whole
 * contract is visible: what is on the right is what appears, top to
 * bottom, left to right.
 */
static void
lists_load(void)
{
    char  buf[128];
    char *tok;
    int   used[N_FIELDS];
    int   i;

    memset(used, 0, sizeof used);
    gtk_clist_clear(GTK_CLIST(g_avail));
    gtk_clist_clear(GTK_CLIST(g_shown));

    strncpy(buf, g_info_fields, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    for (tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        int k = field_index(tok);

        if (k >= 0 && !used[k]) {
            char *row[1];

            row[0] = (char *) g_field_label[k];
            gtk_clist_append(GTK_CLIST(g_shown), row);
            used[k] = 1;
        }
    }

    for (i = 0; i < N_FIELDS; i++) {
        if (!used[i]) {
            char *row[1];

            row[0] = (char *) g_field_label[i];
            gtk_clist_append(GTK_CLIST(g_avail), row);
        }
    }
}

/* Read the Shown list back into the setting, in its current order. */
static void
lists_store(void)
{
    GtkCList *cl = GTK_CLIST(g_shown);
    int n = 0, i;

    g_info_fields[0] = '\0';

    for (i = 0; i < cl->rows; i++) {
        char *text = NULL;
        int   k;

        if (!gtk_clist_get_text(cl, i, 0, &text) || text == NULL)
            continue;

        for (k = 0; k < N_FIELDS; k++) {
            if (strcmp(g_field_label[k], text) == 0) {
                if (n > 0 && n < (int) sizeof g_info_fields - 1)
                    g_info_fields[n++] = ',';
                strncpy(g_info_fields + n, g_field_name[k],
                        sizeof g_info_fields - n - 1);
                n += strlen(g_field_name[k]);
                break;
            }
        }
    }
    g_info_fields[n] = '\0';
}

/* Move the selected row from one list to the other. */
static void
move_between(GtkCList *from, GtkCList *to)
{
    char *text = NULL;
    char *row[1];
    int   sel;

    if (from->selection == NULL)
        return;
    sel = GPOINTER_TO_INT(from->selection->data);

    if (!gtk_clist_get_text(from, sel, 0, &text) || text == NULL)
        return;

    row[0] = text;
    gtk_clist_append(to, row);
    gtk_clist_remove(from, sel);
}

static void on_add(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    move_between(GTK_CLIST(g_avail), GTK_CLIST(g_shown));
}

static void on_remove(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    move_between(GTK_CLIST(g_shown), GTK_CLIST(g_avail));
}

/* Move the selection up or down WITHIN the Shown list - this is the
 * half that makes it an ordering widget rather than a picker. */
static void
nudge(int delta)
{
    GtkCList *cl = GTK_CLIST(g_shown);
    int sel, to;

    if (cl->selection == NULL)
        return;
    sel = GPOINTER_TO_INT(cl->selection->data);
    to  = sel + delta;

    if (to < 0 || to >= cl->rows)
        return;

    gtk_clist_swap_rows(cl, sel, to);
    gtk_clist_select_row(cl, to, 0);
}

static void on_up(GtkWidget *w, gpointer d)   { (void)w;(void)d; nudge(-1); }
static void on_down(GtkWidget *w, gpointer d) { (void)w;(void)d; nudge(1);  }

static void
on_dlg_ok(GtkWidget *w, gpointer d)
{
    int old_zoom = g_zoom;

    (void)w;(void)d;

    lists_store();

    if (GTK_TOGGLE_BUTTON(g_zoom_1)->active)
        g_zoom = 1;
    else
        g_zoom = 2;

    g_track_format =
        GTK_TOGGLE_BUTTON(g_fmt_artist)->active ? 1 : 0;

    /* THE MASK IS A DISPLAY CHOICE, so switching it needs every
     * tile converted again - the ring's pixels change meaning. */
    if (g_mask_btn != NULL) {
        int want = GTK_TOGGLE_BUTTON(g_mask_btn)->active ? 1 : 0;

        if (want != g_border_mask) {
            g_border_mask = want;
            cdg_mark_all(&g_screen);
        }
    }
    if (g_clear_btn != NULL)
        g_clear_on_eject = GTK_TOGGLE_BUTTON(g_clear_btn)->active ? 1 : 0;
    if (g_compact_btn != NULL) {
        int want = GTK_TOGGLE_BUTTON(g_compact_btn)->active ? 1 : 0;

        if (want != g_compact_detached) {
            g_compact_detached = want;
            layout_rows();      /* takes effect now if detached */
        }
    }

    /*
     * REDRAW WHAT THE SETTINGS DESCRIBE. Storing g_track_format and
     * g_info_fields changes nothing on screen by itself - the
     * dropdown and the information line are built once and keep
     * whatever they were given.
     */
    tracks_load(g_disc_tracks, &g_disc);
    info_load(g_disc_cur, &g_disc);

    {
        int i;

        for (i = 0; i < 9; i++)
            if (g_anchor_btn[i] != NULL &&
                GTK_TOGGLE_BUTTON(g_anchor_btn[i])->active) {
                /* The array runs 7,8,9,4,5,6,1,2,3 - screen order -
                 * so the cell number is not the index. */
                static const int cell_of[9] = { 7,8,9, 4,5,6, 1,2,3 };

                g_anchor = cell_of[i];
                break;
            }
        cdg_apply_anchor();
    }

    /*
     * A ZOOM CHANGE RESIZES THE PICTURE, so the image has to go - it
     * is allocated at the old size and gdk_image_new() takes the size
     * at creation. image_make() builds the new one and marks every
     * tile, since a fresh image holds nothing.
     */
    if (g_zoom != old_zoom && g_area != NULL) {
        image_free();
        gtk_drawing_area_size(GTK_DRAWING_AREA(g_area),
                              VIEW_W(g_zoom), VIEW_H(g_zoom));
        if (g_area->window != NULL) {
            image_make(g_area);
            repaint();
        }
        /*
         * THE FRAME MUST SHRINK WITH THE PICTURE.
         *
         * gtk_drawing_area_size() sets a size REQUEST; it does not
         * resize anything by itself. Without a queue_resize the
         * GtkFrame around the area kept its 2x allocation and drew a
         * 600x432 border around a 300x216 picture - the user
         * captured exactly that.
         */
        gtk_widget_queue_resize(g_area);
        if (g_area->parent != NULL)
            gtk_widget_queue_resize(g_area->parent);

        /*
         * AND RE-ANCHOR. The alignment keeps the position it was
         * given, so a picture that was centred at 2x stayed pinned
         * where the larger widget had put it - top-left at 1x.
         */
        cdg_apply_anchor();

        /* A DETACHED WINDOW MUST FOLLOW THE ZOOM. Otherwise 1x
         * leaves a 2x-sized frame with dead space around a small
         * picture. */
        cdg_fit_float();
    }

    /*
     * AND SAVED - design/47 G5. The user's file only, never created
     * here (that is File > Save Configuration's consent, design/36 row
     * 88); the message says which of the three things happened, as
     * the shell's own does since B1.
     */
    {
        struct vlhe_cdg_prefs pr;
        const char *msg = STR_CDG_MSG_OPTIONS_APPLIED;

        memset(&pr, 0, sizeof pr);
        pr.zoom           = g_zoom;
        pr.anchor         = g_anchor;
        pr.track_format   = g_track_format;
        pr.border_mask    = g_border_mask;
        pr.clear_on_eject = g_clear_on_eject;
        pr.compact_detached = g_compact_detached;
        strncpy(pr.info_fields, g_info_fields, sizeof pr.info_fields - 1);
        pr.info_fields[sizeof pr.info_fields - 1] = '\0';
        /* MEMORY ONLY - design/51, 2026-10-01: only the File menu
         * writes. This used to vlhe_commit_user() here. */
        if (vlhe_set_cdg_prefs(&pr) != 0)
            msg = STR_CDG_MSG_OPTIONS_APPLIED_REFUSED;
        else
            msg = STR_CDG_MSG_OPTIONS_APPLIED_SAVE;
        if (g_report)
            g_report(msg);
    }

    gtk_widget_destroy(g_dlg);
    g_dlg = NULL;
}

static void
on_dlg_cancel(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    gtk_widget_destroy(g_dlg);
    g_dlg = NULL;
}

static gint
on_dlg_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;(void)e;(void)d;
    g_dlg = NULL;
    return FALSE;
}

static GtkWidget *
titled_list(const char *title, GtkWidget **list_out)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    GtkWidget *lab = gtk_label_new(title);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *cl = gtk_clist_new(1);

    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
    gtk_clist_set_selection_mode(GTK_CLIST(cl), GTK_SELECTION_SINGLE);
    gtk_clist_set_column_width(GTK_CLIST(cl), 0, 110);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_usize(scroll, 130, 140);
    gtk_container_add(GTK_CONTAINER(scroll), cl);

    gtk_box_pack_start(GTK_BOX(box), lab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    *list_out = cl;
    return box;
}

static void
on_options(GtkWidget *w, gpointer d)
{
    GtkWidget *vbox, *frame, *hbox, *mid, *btn, *row, *inner;
    GtkWidget *right, *bottom;
    GSList    *grp;

    (void)w;(void)d;

    if (g_dlg != NULL) {           /* already open - raise it */
        gdk_window_raise(g_dlg->window);
        return;
    }

    g_dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_dlg), STR_CDG_TITLE_CD_G_VIEWER_OPTIONS);
    gtk_container_set_border_width(GTK_CONTAINER(g_dlg), 8);
    gtk_signal_connect(GTK_OBJECT(g_dlg), "delete_event",
                       GTK_SIGNAL_FUNC(on_dlg_delete), NULL);

    /*
     * TWO COLUMNS - the user, 2026-10-01: "Move the track list shows
     * and Information Line to the right of the other options." The
     * picture options stack on the left, the two list options on the
     * right, and the dialog is wider and shorter rather than a column
     * that ran off a 600-high screen. OK and Cancel stay below both.
     */
    {
        GtkWidget *cols = gtk_hbox_new(FALSE, 8);

        bottom = gtk_vbox_new(FALSE, 8);
        gtk_container_add(GTK_CONTAINER(g_dlg), bottom);
        gtk_box_pack_start(GTK_BOX(bottom), cols, TRUE, TRUE, 0);
        vbox = gtk_vbox_new(FALSE, 8);
        gtk_box_pack_start(GTK_BOX(cols), vbox, FALSE, FALSE, 0);
        right = gtk_vbox_new(FALSE, 8);
        gtk_box_pack_start(GTK_BOX(cols), right, TRUE, TRUE, 0);
    }

    /* --- picture size ------------------------------------------- */
    frame = gtk_frame_new(STR_CDG_FRAME_PICTURE_SIZE);
    inner = gtk_hbox_new(FALSE, 12);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 6);

    g_zoom_2 = vlhe_tipped(gtk_radio_button_new_with_label(NULL, STR_CDG_RADIO_2X_600_X_432), STR_CDG_RADIO_2X_600_X_432_TIP);
    grp = gtk_radio_button_group(GTK_RADIO_BUTTON(g_zoom_2));
    g_zoom_1 = vlhe_tipped(gtk_radio_button_new_with_label(grp, STR_CDG_RADIO_1X_300_X_216), STR_CDG_RADIO_1X_300_X_216_TIP);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(g_zoom == 1 ? g_zoom_1 : g_zoom_2), TRUE);

    gtk_box_pack_start(GTK_BOX(inner), g_zoom_2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), g_zoom_1, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* --- the border mask ---------------------------------------- */
    frame = gtk_frame_new(STR_CDG_FRAME_BORDER);
    inner = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 6);

    /* THE EXPLANATION WENT - the user, 2026-10-01: "Remove the verbose
     * text of the Border option." The template's comment on BorderMask
     * keeps it for whoever reads the file. */
    g_mask_btn = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CDG_CHECK_HIDE_OUTER_EDGE_AS), STR_CDG_CHECK_HIDE_OUTER_EDGE_AS_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_mask_btn),
                                 g_border_mask ? TRUE : FALSE);
    gtk_box_pack_start(GTK_BOX(inner), g_mask_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    frame = gtk_frame_new(STR_CDG_FRAME_WHEN_DISC_EJECTED);
    inner = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 6);

    g_clear_btn = vlhe_tipped(gtk_check_button_new_with_label(STR_CDG_CHECK_CLEAR_PICTURE), STR_CDG_CHECK_CLEAR_PICTURE_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_clear_btn),
                                 g_clear_on_eject ? TRUE : FALSE);
    gtk_box_pack_start(GTK_BOX(inner), g_clear_btn, FALSE, FALSE, 0);

    {
        GtkWidget *n = gtk_label_new(
            STR_CDG_LABEL_OFF_KEEPS_LAST_FRAME);

        gtk_label_set_justify(GTK_LABEL(n), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(n), 0.0, 0.5);
        gtk_label_set_line_wrap(GTK_LABEL(n), TRUE);
        gtk_widget_set_usize(n, 360, -1);
        gtk_box_pack_start(GTK_BOX(inner), n, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* --- where the picture sits: the numpad grid ---------------- */
    frame = gtk_frame_new(STR_CDG_FRAME_PICTURE_POSITION);
    {
        /*
         * NINE CELLS, LAID OUT AS A NUMPAD - the user's design. The
         * table is in SCREEN order (top row first) while the cells
         * are numbered bottom-up, which is what cell_of[] reconciles.
         */
        static const int cell_of[9] = { 7,8,9, 4,5,6, 1,2,3 };
        GtkWidget *tbl = gtk_table_new(3, 3, TRUE);
        GtkWidget *hb  = gtk_hbox_new(FALSE, 8);
        GtkWidget *note;
        GSList    *g = NULL;
        int        i;

        gtk_container_set_border_width(GTK_CONTAINER(hb), 6);

        for (i = 0; i < 9; i++) {
            /* A RADIO BUTTON WITH NO LABEL draws as a small dot,
             * which is the "nine squares with a dot in each" the
             * user described and needs no custom widget. */
            g_anchor_btn[i] = gtk_radio_button_new(g);
            g = gtk_radio_button_group(GTK_RADIO_BUTTON(g_anchor_btn[i]));

            if (cell_of[i] == g_anchor)
                gtk_toggle_button_set_active(
                    GTK_TOGGLE_BUTTON(g_anchor_btn[i]), TRUE);

            gtk_table_attach_defaults(GTK_TABLE(tbl), g_anchor_btn[i],
                                      i % 3, i % 3 + 1,
                                      i / 3, i / 3 + 1);
        }

        gtk_box_pack_start(GTK_BOX(hb), tbl, FALSE, FALSE, 0);

        note = gtk_label_new(
            STR_CDG_LABEL_CONTROLS_STAY_BOTTOM_ONLY);
        gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
        gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(hb), note, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(frame), hb);
    }
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* --- detached: the compact second row ------------------------ */
    frame = gtk_frame_new(STR_CDG_FRAME_WHEN_DETACHED);
    inner = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 6);
    g_compact_btn = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CDG_CHECK_ATTACH_TRACK_LIST_SECOND), STR_CDG_CHECK_ATTACH_TRACK_LIST_SECOND_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_compact_btn),
                                 g_compact_detached ? TRUE : FALSE);
    gtk_box_pack_start(GTK_BOX(inner), g_compact_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* --- track list format -------------------------------------- */
    frame = gtk_frame_new(STR_CDG_FRAME_TRACK_LIST_SHOWS);
    inner = gtk_vbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 6);

    g_fmt_title = vlhe_tipped(gtk_radio_button_new_with_label(NULL,
        STR_CDG_RADIO_TITLE_01_SONG_JOY), STR_CDG_RADIO_TITLE_01_SONG_JOY_TIP);
    grp = gtk_radio_button_group(GTK_RADIO_BUTTON(g_fmt_title));
    g_fmt_artist = vlhe_tipped(gtk_radio_button_new_with_label(grp,
        STR_CDG_RADIO_ARTIST_TITLE_01_PURRS), STR_CDG_RADIO_ARTIST_TITLE_01_PURRS_TIP);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(g_track_format ? g_fmt_artist : g_fmt_title),
        TRUE);

    gtk_box_pack_start(GTK_BOX(inner), g_fmt_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), g_fmt_artist, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(right), frame, FALSE, FALSE, 0);

    /* --- the info row's fields, and their ORDER ------------------ */
    frame = gtk_frame_new(STR_CDG_FRAME_INFORMATION_LINE);
    hbox = gtk_hbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 6);

    gtk_box_pack_start(GTK_BOX(hbox),
                       titled_list(STR_CDG_LABEL_AVAILABLE, &g_avail),
                       TRUE, TRUE, 0);

    /* ONE GAP BETWEEN ALL FOUR, AND THE COLUMN STARTS A BUTTON'S
     * HEIGHT DOWN - the user, 2026-10-01: "Move the Add, Remove, Move
     * up, Move down buttons down a bit maybe one button height down
     * and have them so they have the same gap between them
     * vertically." The spacer is sized from a button's own request,
     * so it is one button tall at the target's font too. */
    mid = gtk_vbox_new(FALSE, 6);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_ADD), STR_CDG_BTN_ADD_TIP);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_add), NULL);
    {
        GtkRequisition req;
        GtkWidget *spacer = gtk_label_new("");

        gtk_widget_size_request(btn, &req);
        gtk_widget_set_usize(spacer, -1, req.height);
        gtk_box_pack_start(GTK_BOX(mid), spacer, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(mid), btn, FALSE, FALSE, 0);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_REMOVE), STR_CDG_BTN_REMOVE_TIP);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_remove), NULL);
    gtk_box_pack_start(GTK_BOX(mid), btn, FALSE, FALSE, 0);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_MOVE_UP), STR_CDG_BTN_MOVE_UP_TIP);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_up), NULL);
    gtk_box_pack_start(GTK_BOX(mid), btn, FALSE, FALSE, 0);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_MOVE_DOWN), STR_CDG_BTN_MOVE_DOWN_TIP);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_down), NULL);
    gtk_box_pack_start(GTK_BOX(mid), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), mid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox),
                       titled_list(STR_CDG_LABEL_SHOWN_IN_ORDER, &g_shown),
                       TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(frame), hbox);
    gtk_box_pack_start(GTK_BOX(right), frame, TRUE, TRUE, 0);

    lists_load();

    /* --- OK / Cancel --------------------------------------------- */
    row = gtk_hbox_new(TRUE, 6);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_OK), STR_SHELL_BTN_OK_TIP);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_dlg_ok), NULL);
    gtk_box_pack_end(GTK_BOX(row), btn, FALSE, FALSE, 0);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_dlg_cancel), NULL);
    gtk_box_pack_end(GTK_BOX(row), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), row, FALSE, FALSE, 0);

    gtk_widget_show_all(g_dlg);

    /* The same size as every other dialog's buttons - the user,
     * 2026-10-01: "the Cancel needs a bit of width and the Okay needs
     * to match width". After show_all, which the helper wants. */
    vlhe_buttons_equalise(row);
}

/* ------------------------------------------------------------------ */
/* Transport actions                                                  */
/* ------------------------------------------------------------------ */

/*
 * THE TRANSPORT WORKS THE ATTACHED DRIVE, and say() reports what each
 * button did or why it could not - a control that silently does
 * nothing is worse than one that says so.
 *
 * CORRECTED 2026-10-05 (the G01 documentation review). This said the
 * page was "NOT WIRED TO A DISC YET" and each button reported what it
 * WOULD do, waiting on VDISC_OP_READ_SUB reaching this process. Both
 * went the other way: the buttons drive the drive through
 * vlhe_cd_play()/pause()/resume()/stop(), and the picture is decoded
 * from the image file itself with vdisc_image_read_sub() (design/34
 * section 6e2) - READ_SUB is served by vdiscd but nothing issues it.
 */
static void say(const char *what) { if (g_report) g_report(what); }

/*
 * WHICH DRIVE THE TRANSPORT ACTS ON - design/34 6e2.
 *
 * THE FIRST ONE WITH AUDIO, and that is a choice rather than an
 * oversight. This page is a PLAYER, so a drive holding a data-only
 * image is not a candidate however recently it was attached; a
 * machine with one disc in it - the ordinary case - has exactly one
 * answer either way.
 *
 * -1 WHEN THERE IS NONE, which every caller reports rather than
 * acting on. A transport button that silently does nothing is the
 * thing the old stubs were written to avoid.
 *
 * RE-ASKED PER PRESS rather than cached. A disc can be attached or
 * ejected while this page is open, and the CD page does exactly
 * that; a cached index would act on a drive that had changed under
 * it.
 *
 * THE USER'S CHOICE FIRST - design/31 C9, 2026-10-03. The CD page's
 * "CD+G viewer" radio sets [CDG Viewer] Drive (default 0); while that
 * drive holds a disc with audio it is the answer, whatever is in a
 * lower drive. Only when it does not - empty, or a data disc - does
 * the first-with-audio rule above apply, so ejecting the chosen disc
 * still lets the viewer follow another, as design/52 AR1's test has
 * it. Every drive can play since AR1, so the choice is real.
 */
static int
audio_drive(void)
{
    struct vlhe_drive dv[VLHE_MAX_DRIVE];
    int n, i, want;

    n = vlhe_drives(dv, VLHE_MAX_DRIVE);
    want = vlhe_cdg_drive();
    for (i = 0; i < n; i++)
        if (dv[i].index == want && dv[i].attached
            && dv[i].audio_tracks > 0)
            return want;
    /*
     * AND ONLY IF FOLLOWING ANY DISC - [CDG Viewer] FollowAnyDisc, on
     * by default (2026-10-03). The user: "with auto detection CD+G
     * player picks slot 0. With auto detection off. It should pick
     * slot 2 and be no disc until you select a different one." Off,
     * the chosen drive is the only answer, empty or not.
     */
    if (!vlhe_cdg_follow_any())
        return -1;
    for (i = 0; i < n; i++)
        if (dv[i].attached && dv[i].audio_tracks > 0)
            return dv[i].index;
    return -1;
}

/*
 * OPEN THE ATTACHED IMAGE FOR OUR OWN READING, or close what we
 * have. Called when the drive we act on changes, beside the TOC
 * read - the two answer the same question, "what disc is this".
 *
 * A FAILURE IS NOT FATAL AND IS NOT REPORTED HERE. A disc whose
 * image we cannot open still plays - the daemon has it - it simply
 * shows no picture, and saying so on every poll would be noise.
 * The caller decides whether to mention it once.
 */
static void
image_follow(int drv)
{
    struct vlhe_drive dv[VLHE_MAX_DRIVE];
    char full[VLHE_PATH_MAX];
    int n, i;

    if (g_img_open) {
        vdisc_image_close(&g_img);
        g_img_open = 0;
    }
    g_sub_lba = -1;
    cdg_reset(&g_screen);

    /*
     * A NEW DISC IS NOT PAUSED - 2026-09-29, and leaving this out
     * was a real trap.
     *
     * The state lived only in the three transport buttons, so
     * pausing and then SWAPPING THE IMAGE left `g_transport' at
     * PAUSED against a drive with nothing paused. Play then took
     * the resume branch and `vlhe_cd_resume()' had nothing to
     * resume, so the button appeared dead. The user found it, and
     * got unstuck by skipping to the next track and back - because
     * the track buttons call `vlhe_cd_play()' directly and never
     * consult the state.
     *
     * The transport belongs to the disc, so it resets with it.
     */
    transport_set(TRANSPORT_STOPPED);

    if (drv < 0)
        return;

    n = vlhe_drives(dv, VLHE_MAX_DRIVE);
    for (i = 0; i < n; i++)
        if (dv[i].index == drv && dv[i].attached && dv[i].image[0] != '\0')
            break;
    if (i >= n) {
        /* SAY SO - 2026-09-27. A silent return here looks exactly
         * like a disc with no graphics, and telling the two apart
         * cost several rounds on the target. */
        trace("drive %d has no attached image among %d drive(s)", drv, n);
        return;
    }

    /*
     * THE FULL PATH, ASKED FOR SEPARATELY - 2026-09-27, and this is
     * the bug that made the page look dead.
     *
     * `dv[i].image' COMES FROM `status', WHICH TRUNCATES TO 28
     * CHARACTERS (vdiscd.c cmd_status). Fine for showing which disc
     * is in a drive; useless for opening one. On 86Box
     * /mnt/discs/infosoc/infosoc.ccd arrived as
     * `/mnt/discs/infosoc/infosoc.i' and every symptom followed from
     * the open failing on it.
     *
     * FALL BACK TO THE SHORT ONE rather than refusing: on a drive
     * whose path is under 28 characters they are the same string,
     * and a daemon too old to know the `image' verb should still
     * work for those.
     */
    if (vlhe_drive_image(drv, full, sizeof full) != 0) {
        strncpy(full, dv[i].image, sizeof full - 1);
        full[sizeof full - 1] = '\0';
        trace("drive %d: no `image' reply - falling back to the"
              " status path `%s'", drv, full);
    }

    trace("drive %d image `%s' (daemon says %d track(s), %d audio)",
          drv, full, dv[i].tracks, dv[i].audio_tracks);

    if (vdisc_image_open(&g_img, full) == 0) {
        g_img_open = 1;

        /*
         * WHAT WE OPENED, AS WE SEE IT - and it is a SECOND opinion.
         * The daemon parsed this image too and its count is on the
         * line above; if the two disagree, the GUI's own open is at
         * fault rather than the disc.
         */
        trace("opened - %d track(s), lead-out %d, cd-text %s",
              g_img.n_tracks, g_img.leadout_lba,
              g_img.text.present ? "yes" : "no");

        /*
         * AND ITS CD-TEXT, WHICH THE IMAGE ALREADY CARRIES.
         *
         * `backend_cue.c:165' parses TITLE, PERFORMER, SONGWRITER,
         * COMPOSER and ARRANGER at both disc and track level, into
         * `img->text' (`image.h:105'). So a .cue disc arrives here
         * with its names already decoded and this page had been
         * showing a FIXTURE over the top of them.
         *
         * AN AUDIO CD MAY HAVE CD-TEXT AND USUALLY HAS NO CD+G -
         * the two are unrelated. CD-TEXT rides the lead-in, once
         * per disc (`cdtext.h:5'); CD+G rides the audio tracks at
         * 300 packets a second. A plain album with track titles is
         * the ordinary case for this page's track list, and the
         * karaoke disc is the exception.
         */
        if (g_img.text.present) {
            g_disc        = g_img.text;
            g_disc_tracks = g_img.n_tracks;
            g_disc_cur    = 1;
            tracks_load(g_disc_tracks, &g_disc);
            info_load(g_disc_cur, &g_disc);
        } else {
            /*
             * NO CD-TEXT - TRY A CACHED CDDB RECORD, the user's
             * order: "cd-text over cddb but if no cdtext try cddb".
             *
             * CD-TEXT WINS BECAUSE IT CAN SAY MORE. It carries
             * PERFORMER at track level and CDDB structurally
             * cannot - one `DTITLE=artist / title' for the whole
             * disc and `TTITLEn' for the names (design/34 6d).
             * That is exactly the case the user raised for their
             * own compilations, where every track is a different
             * artist.
             *
             * AND WE LOOK UP NOTHING. The record has to be on this
             * machine already, left by KsCD or grip. A disc nobody
             * has fetched shows <Unknown>, which is KsCD's own
             * convention (`kscd.cpp:1295') and what a disc with no
             * names genuinely presents.
             */
            int starts[CDG_MAX_TOC];
            int i, n = 0;

            memset(&g_disc, 0, sizeof g_disc);

            for (i = 0; i < g_img.n_tracks && i < CDG_MAX_TOC; i++)
                starts[n++] = g_img.track[i].start_lba;

            if (n > 0) {
                unsigned long id =
                    vlhe_cddb_discid(starts, n, g_img.leadout_lba);

                /* A FAILURE IS SILENT AND LEAVES THE STRUCT ZEROED,
                 * which is the <Unknown> case - not an error worth
                 * a message, since most discs will not be cached. */
                (void) vlhe_cddb_lookup(id, &g_disc);
            }

            g_disc_tracks = g_img.n_tracks;
            g_disc_cur    = 1;
            tracks_load(g_disc_tracks, &g_disc);
            info_load(g_disc_cur, &g_disc);
        }

        /*
         * HAS THIS DISC ANY GRAPHICS AT ALL? - asked once, here.
         *
         * `read_sub()' RETURNS 0 ON SUCCESS, NOT A BYTE COUNT -
         * 2026-09-27, and every caller on this page had it wrong.
         *
         * All four tested `> 0' or `<= 0', so a SUCCESSFUL read read
         * as failure and NO DISC EVER SHOWED GRAPHICS, whatever its
         * format. Measured: Smash Hits is a 2448 rip with the
         * subchannel embedded, `read_sub' returns 0, and this page
         * reported `graphics none' and collapsed the picture.
         *
         * The contract is in image.h:188 and the host test
         * (`test_image_sub.c:90') had it right all along, which is
         * why nothing caught it - the test exercises the image
         * layer, not this page.
         *
         * `vdisc_image_read_sub()' returns -2 when the file's
         * stride has no room for subchannel, which is a DIFFERENT
         * answer from 96 bytes of zeros (`image.c', and design/27
         * 4a measured QUAKE106.sub as exactly the second case: the
         * space faithfully recorded with nothing in it).
         *
         * Both mean no picture. Knowing which at ATTACH rather than
         * per poll is what lets the viewer stop working entirely on
         * an ordinary audio CD - see `g_has_graphics'.
         */
        {
            static unsigned char probe[96];
            int first = (g_img.n_tracks > 0)
                      ? g_img.track[0].start_lba : 0;
            int k, any = 0;

            /*
             * WALK FORWARD UNTIL SOMETHING IS THERE - 2026-09-28,
             * and looking only at the track start is why a disc
             * with graphics reported `graphics none' on target.
             *
             * THE START OF A DISC IS SILENT. Measured on the
             * Information Society `.cdg': sectors 37 and 69 are
             * ENTIRELY ZERO and content does not begin until lba
             * 150. This probe read 1 sector then 32 from
             * `track[0].start_lba' - both inside that silence - and
             * concluded there were no graphics, so `cdg_set_hidden()'
             * collapsed the picture on a disc that had one.
             *
             * SAME TRAP AS `sniff_layout()' HAD, in a different
             * function: a span at the track start cannot decide
             * anything, because every disc is quiet there. That one
             * was fixed the same day and this one was missed - the
             * two now walk the same way.
             *
             * CHEAP: it stops at the FIRST span carrying anything,
             * which on a real disc is the second or third. The
             * budget is the same CDG_SNIFF_SPANS the layout sniff
             * uses, ~14 seconds of audio, far more lead-in silence
             * than any disc has.
             */
            g_has_graphics = 0;
            {
                int span_i;

                for (span_i = 0; span_i < CDG_SNIFF_SPANS && !any;
                     span_i++) {
                    static unsigned char span[96 * 32];
                    int at = first + span_i * 32;

                    if (vdisc_image_read_sub(&g_img, at, 32, span) != 0)
                        break;          /* no subchannel, or past the end */
                    for (k = 0; k < 96 * 32; k++)
                        if (span[k] != 0) { any = 1; break; }
                }
                g_has_graphics = any;
                (void) probe;
            }

            /*
             * COLLAPSE ONLY WHEN DETACHED - REVERTED 2026-09-27 at
             * the user's instruction.
             *
             * THIS USED TO COLLAPSE THE PICTURE WHENEVER A DISC HAD
             * NO GRAPHICS, in the attached page too, and the result
             * was a window that rearranged itself: *"the controls
             * moved to the top the black screen is gone"*. The
             * embedded page has a fixed shape and should keep it.
             *
             * THE USER'S RULE: *"I already have a show hide button
             * for when it was detached. So when detached if there is
             * nothing detected that should auto hide. Thats all and
             * showing it can still stay the black image."* A floating
             * window with nothing in it is worth dismissing on its
             * own; a panel in a page is not.
             *
             * `g_float' IS THE TEST rather than a flag of our own -
             * it is the detached window and is NULL when the picture
             * is in the page, so the two cannot disagree.
             *
             * AND Show STILL WORKS EITHER WAY. Nothing here forces
             * the mode back, so someone who wants the empty frame -
             * to check a disc really has no graphics - can have it,
             * and it stays the black image.
             */
            if (g_float != NULL)
                cdg_set_hidden(!g_has_graphics);

            trace("graphics %s%s - track list has %d entr%s",
                  g_has_graphics ? "FOUND" : "none",
                  g_has_graphics ? ""
                                 : (g_float != NULL
                                    ? " (detached window hidden)"
                                    : " (picture kept - attached)"),
                  g_disc_tracks, g_disc_tracks == 1 ? "y" : "ies");
        }
    } else {
        /*
         * THE OPEN FAILED, AND IT USED TO SAY NOTHING - 2026-09-27.
         *
         * The comment at the top of this function is right that a
         * failure is not fatal: the daemon has the disc and it still
         * plays. But SILENCE here is indistinguishable from a disc
         * with no graphics, and on the target that ambiguity cost
         * several rounds of elimination. The daemon reads this same
         * path successfully, so a failure HERE is worth knowing
         * about - most likely a permission or a relative-path
         * problem particular to this process.
         */
        trace("CANNOT OPEN `%s' - no picture and no track names;"
              " the daemon still plays it", full);
    }
}

/*
 * DECIDE THE SUBCHANNEL LAYOUT ONCE, from the disc itself.
 *
 * `design/34' section 6a: our two karaoke discs DISAGREE - 69.5% of
 * packets decoding directly on one against 0.0% on the other - and
 * the CUE says the stride but not the format, so cdemu's guess of
 * PW96_INTERLEAVED for every 2448-byte track is wrong for Smash
 * Hits. `cdg_sniff()' tries both over a span and says which yields
 * more valid packets.
 *
 * A SPAN FROM WHERE THE MUSIC IS, not from sector 0. The lead-in of
 * a mixed disc is a data track with no graphics, where both layouts
 * decode to nothing and the sniff would be a coin toss.
 */
static void
sniff_layout(int lba)
{
    static unsigned char buf[96 * 32];
    int span, direct, unpacked;

    if (!g_img_open)
        return;
    trace("layout sniff starts at lba %d", lba);   /* D60: its cost */

    /*
     * SNIFF UNTIL THERE IS EVIDENCE, NOT AT ONE FIXED SPOT -
     * 2026-09-27, and sniffing at one spot is why Smash Hits
     * detected graphics and drew nothing.
     *
     * THE START OF A TRACK IS SILENT, AND SILENCE IS A TIE.
     * Measured on that disc, 32 sectors at a time:
     *
     *     lba  32: direct=  0  unpacked=  0  -> tie, PW96
     *     lba  64: direct= 91  unpacked=  0  -> RW96
     *     lba 100: direct=128  unpacked=  0  -> RW96
     *
     * `cdg_sniff()' breaks a tie towards PW96_INTERLEAVED, and its
     * own comment says why that is safe: *"a tie means neither
     * layout is carrying graphics and the answer does not
     * matter."* **IT MATTERS HERE BECAUSE THE ANSWER IS CACHED.**
     * `decode_to()' sniffs once on the first decode - which is at
     * the track start, the one place with no packets - and every
     * later sector is then decoded with the wrong layout, so the
     * screen fills with nothing.
     *
     * SO WALK FORWARD UNTIL A LAYOUT WINS. A span with packets
     * decides it; a span with none is skipped rather than allowed
     * to answer. `CDG_SNIFF_SPANS` of 32 sectors is ~14 seconds of
     * audio, which is far more lead-in silence than any disc has,
     * and the loop stops at the first span that carries anything.
     *
     * A DISC WITH NO GRAPHICS AT ALL falls out of the loop with the
     * layout untouched - which is correct, and `g_has_graphics'
     * has already said so by then.
     */
    for (span = 0; span < CDG_SNIFF_SPANS; span++) {
        int at = lba + span * 32;
        int i, k;

        if (vdisc_image_read_sub(&g_img, at, 32, buf) != 0) {
            trace("layout sniff stopped at lba %d: no subchannel there", at);
            return;                     /* no subchannel, or past the end */
        }

        /* THE SAME TEST cdg_sniff() MAKES, so the two cannot
         * disagree about what "carries packets" means - but counted
         * here so an empty span can be told from a decided one,
         * which a single int return cannot express. */
        direct = unpacked = 0;
        for (i = 0; i < 32; i++) {
            const unsigned char *p = buf + i * VDISC_SUB_SIZE;
            unsigned char packs[CDTEXT_RW_OUT];

            for (k = 0; k < 96; k += CDG_PACKET_SIZE)
                if ((p[k] & 0x3f) == CDG_COMMAND)
                    direct++;
            cdtext_rw_unpack(p, packs);
            for (k = 0; k < CDTEXT_RW_OUT; k += CDG_PACKET_SIZE)
                if ((packs[k] & 0x3f) == CDG_COMMAND)
                    unpacked++;
        }

        if (direct == 0 && unpacked == 0)
            continue;                   /* silence - it cannot decide */

        g_sub_layout = cdg_sniff(buf, 32);
        trace("layout sniffed at lba %d: %s (direct %d, unpacked %d)",
              at, g_sub_layout == CDG_SUB_RW96 ? "RW96" : "PW96",
              direct, unpacked);
        return;
    }
    trace("layout sniff: no packets in %d spans from lba %d - unchanged",
          CDG_SNIFF_SPANS, lba);
}

/*
 * DECODE FORWARD TO `target'.
 *
 * THE SCREEN AT SECTOR N DEPENDS ON EVERY PACKET BEFORE IT, so this
 * walks rather than jumps - which is what every reference player
 * does. OpenKJ rewinds to black and replays when asked to go
 * backwards (`cdgfilereader.cpp:87'); cdgdeck calls the same thing
 * S_ENHANCED and offers S_DIRECT as the fast, visibly-wrong
 * alternative that VLC takes (`cd-refs/cdgdeck/cdg.h:70').
 *
 * BACKWARDS MEANS STARTING AGAIN. A seek back 30 seconds is 2250
 * sectors, and there is no way to unwind a TILE_BLOCK.
 *
 * BOUNDED PER CALL. A poll that had to replay four minutes would
 * freeze the GUI; this does a fixed budget and returns, so the
 * picture catches up over several polls instead of blocking one.
 * The budget is generous relative to a quarter-second poll (75
 * sectors of real time) and small relative to a whole disc.
 */
#define CDG_CATCHUP_MAX 4096            /* sectors per call */

/* SECTORS PER READ in the catch-up walk - 64 x 96 = 6 KB, one seek
 * instead of 64. See decode_to(). */
#define CDG_DECODE_BLK 64

/*
 * HOW CLOSE TO A TRACK'S START COUNTS AS "STARTING THAT TRACK".
 *
 * A play lands exactly on the track start, but the first poll that
 * SEES it is up to CDG_POLL_MS late and the module's own position
 * lags a little behind that. Two seconds is comfortably more than
 * either and comfortably less than any deliberate seek - the
 * transport's own step is 30 s.
 */
#define CDG_PLAY_SLOP (2 * 75)          /* sectors */

static void
decode_to(int target, int track)
{
    int budget = CDG_CATCHUP_MAX;

    if (!g_img_open || target < 0)
        return;

    if (g_sub_lba < 0 || target < g_sub_lba) {
        /*
         * REWIND TO THE TRACK'S START, NOT THE DISC'S - 2026-09-28.
         *
         * THIS USED TO GO BACK TO SECTOR 0 and walk forward at
         * CDG_CATCHUP_MAX per poll. Starting KsCD at track 5 of the
         * Information Society disc - lba 76882 - therefore took
         * nineteen polls before anything was drawn, and the user
         * measured TWENTY-FIVE SECONDS. Next-track took up to ten.
         *
         * AND IT BUYS NOTHING, because a CD+G track re-establishes
         * the whole screen at its head. Measured on both example
         * discs rather than assumed - the first MEMORY_PRESET and
         * LOAD_CLUT after each track start:
         *
         *     Smash Hits     tracks 2-6: preset +43, clut +42..43
         *     Info Society   tracks 2-6: preset +51, clut +50..51
         *
         * So everything decoded before the track start is overwritten
         * within about two-thirds of a second of it. That is the
         * format's own convention - a listener can start on any
         * track, so a track cannot depend on the one before it.
         *
         * NOT A GUARANTEE, THOUGH. A disc that carries state across a
         * boundary would show stale pixels until its next preset,
         * which is a cosmetic fault lasting under a second and is
         * worth twenty-five seconds of waiting. If one ever turns up,
         * the fix is to walk from the track start WITHOUT rendering
         * rather than to go back to zero.
         */
        /*
         * FROM `g_img', NOT `g_toc' - CORRECTED 2026-09-28, THE SAME
         * DAY THIS WAS WRITTEN, BECAUSE THE FIRST VERSION WAS INERT.
         *
         * `g_ntoc' is filled by `transport_drive()', which runs only
         * when one of OUR transport buttons is pressed. A user
         * playing through KsCD never presses one, so `g_ntoc' was 0,
         * the loop matched nothing, and `from' stayed at the old
         * lba 0 - the fix did nothing in exactly the case it was
         * written for. The user spotted it: "it quickly walked from
         * lba 0 but then caught up quickly", which is the BLOCK
         * READS below working while this did not.
         *
         * `g_img' IS THE RIGHT SOURCE and always was. The page opens
         * the image itself for the graphics (design/34 6e2), so
         * `g_img.track[]' is populated whenever there is a picture to
         * draw - the same condition `g_img_open' above already
         * guards. It also cannot disagree with the decoder, because
         * it IS what the decoder reads.
         */
        int from = (target > 0) ? 0 : target;
        int start = -1;
        int i;

        /* WHICH TRACK IS `target' IN? The subchannel hands us the
         * number, so this is an index rather than a search - but it
         * is checked, because a report can name a track the image
         * does not have. */
        if (track > 0 && track <= g_img.n_tracks)
            start = g_img.track[track - 1].start_lba;
        else
            for (i = 0; i < g_img.n_tracks; i++)
                if (g_img.track[i].start_lba <= target
                    && (i + 1 >= g_img.n_tracks
                        || g_img.track[i + 1].start_lba > target)) {
                    start = g_img.track[i].start_lba;
                    break;
                }

        if (start >= 0)
            from = start;

        /*
         * A PLAY WALKS NOTHING; ONLY A SEEK WALKS - the user's
         * point, 2026-09-28: *"the next track or previous track are
         * plays at a certain track so dont need walking"*.
         *
         * TWO DIFFERENT OPERATIONS REACH HERE AND THE CODE COULD NOT
         * TELL THEM APART. Starting track 13 of Smash Hits took OVER
         * A HUNDRED SECONDS because a play was doing seek-shaped
         * work: reset, then walk every sector from somewhere earlier.
         *
         * AND THE CALLER CANNOT TELL US WHICH. KsCD, grip and any
         * period player issue `CDROMPLAYMSF' with an address and
         * nothing else - there is no "this is a track change" in the
         * protocol, and requiring one would defeat the point of
         * vdisc. So it is INFERRED from what the drive reports, which
         * works identically however playback was started.
         *
         * THE TEST: is the position at the head of its track? A play
         * is, within a poll's worth of slop; a seek is not, because
         * the transport's step is 30 s. On a play the screen is
         * being started fresh and the track's own MEMORY_PRESET
         * rebuilds it within ~50 sectors, so there is nothing to
         * replay - jump to the start and decode forward normally.
         *
         * WHAT IT COSTS ON A DISC WITHOUT THAT CONVENTION: the
         * previous frame stays on screen for under a second before
         * the new track clears it. Both example discs preset within
         * 43 and 51 sectors (measured); a disc that carried state
         * across a boundary would show one stale frame briefly.
         */
        cdg_reset(&g_screen);
        g_sub_lba = from;
        if (start >= 0 && target - start < CDG_PLAY_SLOP)
            g_sub_lba = target;         /* a play: nothing to walk */
        sniff_layout(target);
    }

    /*
     * READ IN BLOCKS, NOT ONE SECTOR AT A TIME - 2026-09-28, the
     * other half of the catch-up cost.
     *
     * `vdisc_image_read_sub()' seeks per call, and on a 2448 image it
     * also carries three sectors of de-interleave history - so 4096
     * calls a poll was 4096 seeks. A block amortises both.
     *
     * 64 SECTORS is one 6 KB buffer of RW96 and comfortably inside
     * the ~14 KB this function already has on the stack elsewhere.
     * The remainder is read as a short block rather than specially.
     */
    while (g_sub_lba < target && budget > 0) {
        static unsigned char blk[CDG_DECODE_BLK * 96];
        int want = target - g_sub_lba;
        int i;

        if (want > CDG_DECODE_BLK)
            want = CDG_DECODE_BLK;
        if (want > budget)
            want = budget;

        if (vdisc_image_read_sub(&g_img, g_sub_lba, want, blk) != 0) {
            /* NO SUBCHANNEL HERE - a 2352 image, or past the end.
             * Stop rather than spin: there is nothing to decode and
             * the next poll would try the same sector again. */
            g_sub_lba = target;
            return;
        }
        for (i = 0; i < want; i++)
            cdg_sector(&g_screen, blk + i * 96, g_sub_layout);
        g_sub_lba += want;
        budget    -= want;
    }
}

/*
 * THE POLL - where the picture actually happens.
 *
 * FIFTEEN TIMES A SECOND. Each tick asks the kernel where playback
 * is and decodes the gap.
 *
 * THIS WAS 250 ms - FOUR A SECOND - AND THE REASONING WAS THAT
 * "text appears a syllable at a time and 250 ms is below what
 * anyone reads as lag". That is true of LAG and wrong about
 * GRANULARITY, which is what the user actually saw: a lyric line
 * takes about a second to arrive as a stream of TILE_BLOCKs, so at
 * 4 Hz it lands in FOUR VISIBLE INSTALMENTS where a reference
 * player (CDGDeck, karadeo.com) animates it. Their words: "ours
 * kinda draws it in blocks like 4 chunks on the screen".
 *
 * 67 ms IS FIFTEEN INSTALMENTS INSTEAD OF FOUR. The same tiles in
 * the same order, in finer steps.
 *
 * IT ONLY BECAME AFFORDABLE ON 2026-09-29. At 4 Hz the old
 * full-screen rectangle repaint cost 15-30% of the CPU on 86Box at
 * 2x zoom (X server plus ours), so 15 Hz would have been 55-110%
 * and did not fit. After the tile map (9eddf2d), the GdkImage
 * (96955c2) and hoisting the depth switch out of the pixel loop
 * (3e653fd) the same measurement is about 4% - X at ~4% and
 * vlhe.gtk below top's reporting threshold - so 15 Hz is roughly
 * 15%.
 *
 * MEASURED ON 86Box, AND THE SLOW MACHINES ARE UNCHECKED. The P1
 * (Pentium 233) is untestable at the moment; the Acer has a real
 * Mach64 rather than an emulated one. If this proves too dear
 * there, it is one number.
 *
 * IT ASKS THE KERNEL, NOT THE DAEMON. `vlhe_cd_position()' reads
 * CDROMSUBCHNL, which the module answers from the CDDA child's own
 * reports - the daemon never sees them (design/34 6e2).
 *
 * AND THE RESOLUTION IS THE CHILD'S REPORTING INTERVAL, NOT OURS.
 * `vdiscd_audio.c' REPORT_FRAMES was 75 - one position per SECOND
 * of audio - so the picture advanced in one-second steps however
 * often this polled, and the lyric strip visibly stepped where a
 * reference player flowed. **CD+G IS 300 PACKETS A SECOND**, and
 * the lyrics are the one region changing continuously, so they are
 * where a coarse cursor shows.
 *
 * FIXED AT THE SOURCE, 2026-09-29: REPORT_FRAMES follows this
 * poll - it is 5 frames, a fifteenth of a second. THE TWO ARE A
 * PAIR AND MUST MOVE TOGETHER. A poll faster than the cursor finds
 * it unmoved and decodes nothing, so the picture would step at the
 * CURSOR's rate however often this ran, with the extra wakeups
 * pure waste.
 *
 * NOTHING HAPPENS WHEN STOPPED, which is what makes this cheap: one
 * ioctl on a node we open and close, and no decode at all.
 */
#define CDG_POLL_MS 67          /* 15 Hz; REPORT_FRAMES must match */

/*
 * THE TIME READOUT. See `g_time' for why it exists.
 *
 * ELAPSED WITHIN THE TRACK, not absolute on the disc, because that
 * is what a CD player shows and what a user checks against the case.
 * The track's start comes from the image's own TOC, so a disc whose
 * first track does not begin at 0 is right without a special case.
 *
 * NOT vdisc_lba_to_msf() - THAT ADDS THE 150-SECTOR OFFSET, which is
 * correct for an MSF address on the wire and wrong for a duration.
 * `msf.h' says so at the top. Two seconds is small enough to look
 * like a rounding error and survive review, which is exactly why it
 * is worth naming here.
 *
 * REDRAWN ONLY WHEN THE TEXT CHANGES. The poll runs four times a
 * second and the text changes once; `gtk_label_set_text' queues a
 * resize on a GtkLabel whether or not the string differs, and on a
 * P1 that is four needless relayouts a second under the picture.
 */
static void
time_show(int lba, int track, int playing)
{
    char buf[64];
    static char last[64];
    int rel = -1;

    if (g_time == NULL)
        return;

    /*
     * PAUSED KEEPS THE TIME, AND THAT IS KsCD'S BEHAVIOUR.
     *
     * `playing' is `audiostatus == CDROM_AUDIO_PLAY', so a paused
     * drive reports 0 and this printed `--:--' - the clock blanking
     * the moment you pause, which is not what any CD player does.
     *
     * KsCD is explicit (`kscd.cpp:1327'): its PAUSED case sets the
     * status label and DOES NOT call setLEDs(), so the display
     * holds whatever it last showed. Only the STOPPED case (:1334)
     * does `setLEDs("--:--")'.
     *
     * So the position is shown whenever there IS one, and the
     * distinction that matters is stopped versus not. A paused
     * drive still reports its lba through CDROMSUBCHNL - it simply
     * stops advancing.
     */
    if ((playing || g_transport == TRANSPORT_PAUSED) && lba >= 0) {
        int start = 0;

        if (track > 0 && track <= g_img.n_tracks)
            start = g_img.track[track - 1].start_lba;
        rel = lba - start;
        if (rel < 0)
            rel = 0;            /* pregap, or a TOC we cannot trust */
    }

    if (rel < 0)
        strcpy(buf, STR_CDG_LABEL_TEXT);
    else
        sprintf(buf, "%d:%02d", rel / 75 / 60, (rel / 75) % 60);

    if (strcmp(buf, last) == 0)
        return;
    strcpy(last, buf);
    gtk_label_set_text(GTK_LABEL(g_time), buf);
}

static gint
on_poll(gpointer data)
{
    int lba, track = 0, playing = 0;
    static int follow_tick;

    (void)data;

    if (!g_active)
        return TRUE;

    /*
     * FOLLOW THE DISC ONCE A SECOND - design/47 G2, 2026-10-01.
     * follow_disc() ran only when the page was shown, a transport
     * button was pressed or a track picked, so a disc changed from
     * the CD page or from KsCD, or detached, left this page decoding
     * the OLD image against the new disc with the old track list.
     * follow_disc() itself compares drive and image and reloads only
     * on a change, so the cost of asking is one status round trip a
     * second - the CD page makes three.
     *
     * BEFORE THE DRIVE TEST BELOW, or a page that lost its disc could
     * never find the next one.
     */
    if (++follow_tick >= 1000 / CDG_POLL_MS) {
        follow_tick = 0;
        (void) follow_disc();
    }

    if (g_toc_drive < 0)
        return TRUE;

    /*
     * AN AUDIO CD COSTS NOTHING - the user's ask, and it is the
     * COMMON case rather than an optimisation for a rare one. Most
     * discs have no CD+G; this returns before the ioctl, so no
     * sector is read, nothing is decoded and nothing repaints.
     *
     * DECIDED AT ATTACH, not here, because the test is a read and
     * this runs four times a second.
     */
    /*
     * THE TRACK SYNC RUNS FIRST, ABOVE THE GRAPHICS TEST - 2026-09-27.
     *
     * THE DROPDOWN DID NOT FOLLOW THE PLAYING TRACK, the user:
     * *"The drop down does not match the currently playing track. I
     * dont know if kscd handles that."*
     *
     * IT DOES, AND UNCONDITIONALLY - `kscd.cpp:1304' and `:1309',
     * `setCurrentItem(cur_track - 1)' in BOTH branches of its status
     * update, new disc or not. So this is the reference behaviour
     * rather than an invention.
     *
     * AND IT MUST NOT SIT BELOW `g_has_graphics', which is why this
     * is above the early return: an ordinary audio CD has no
     * graphics and returns there, and a player's track display is
     * exactly as wanted on a disc with no picture.
     */
    lba = vlhe_cd_position(g_toc_drive, &track, &playing);

    if (playing && track > 0 && track != g_disc_cur) {
        g_disc_cur = track;
        tracks_select(track);
        info_load(g_disc_cur, &g_disc);
    }

    /*
     * THE BUTTON FOLLOWS THE DISC - design/47 G3. `g_transport' was
     * written only by this page's own presses, so the disc ending, or
     * KsCD starting or stopping it, left the glyph wrong: a pause
     * glyph after the end, or Play restarting a track already
     * playing. `playing' here is audiostatus == PLAY, so a pause is
     * indistinguishable from a stop through this call (transport_set's
     * header) - which is why a PAUSED state is left alone when the
     * disc is not playing, and corrected only when it IS.
     */
    if (playing && g_transport != TRANSPORT_PLAYING)
        transport_set(TRANSPORT_PLAYING);
    else if (!playing && g_transport == TRANSPORT_PLAYING)
        transport_set(TRANSPORT_STOPPED);

    /* THE CLOCK, BEFORE THE GRAPHICS TEST - an audio CD with no
     * CD+G returns below, and its time is worth showing too. */
    time_show(lba, track, playing);

    if (!g_has_graphics)
        return TRUE;

    /*
     * AND SAY WHY WHEN THE DECODE IS SKIPPED - 2026-09-28.
     *
     * THIS WAS THE ONLY UNLOGGED DECISION ON THE PAGE, and it is
     * the one that fired. A session on 2026-09-28 played two CD+G
     * discs with no picture; the log showed `graphics FOUND' on
     * both and then nothing at all, and no amount of reading could
     * say whether the position was unavailable (`lba < 0') or the
     * disc reported itself stopped (`!playing') - which are
     * different faults with different fixes.
     * `tests/logs/2026-09-28-cdg-two-discs-no-graphics/'.
     *
     * RATE-LIMITED TO ROUGHLY ONE A SECOND. The poll runs four
     * times a second and a stuck page would otherwise write four
     * lines a second into DAEMON.LOG for as long as it sat there -
     * which is the flood `vsound_ratelimit' exists for elsewhere.
     * One a second is enough to see it is stuck and cheap enough to
     * leave on.
     *
     * AND ONLY WHILE IT IS STUCK. The counter resets as soon as the
     * decode runs, so a healthy page is silent and the first line
     * after it goes quiet is the interesting one.
     */
    if (lba < 0 || !playing) {
        /*
         * ONLY WHEN IT IS A FAULT - design/47 G4, 2026-10-01. This
         * wrote a line a second, with an fopen/fclose each, for as
         * long as a graphics disc sat STOPPED or PAUSED on this page -
         * which is not the stall the line was written for, and the
         * words ("not reporting a position") were wrong for a pause
         * that reports one perfectly well. A disc this page knows to
         * be stopped or paused is silent; a disc the page believes is
         * playing that gives no position is the stall, and that is
         * still said once a second.
         */
        /* THE FIRST IS AN EVENT; THE ONCE-A-SECOND REPEATS ARE
         * DETAIL (level 2) - 2026-10-03. */
        if (g_transport == TRANSPORT_PLAYING
            && (g_nodecode_said++ % (1000 / CDG_POLL_MS)) == 0) {
            if (g_nodecode_said == 1)
                trace("no decode: lba %d, playing %d, track %d"
                      " - playing by our account, but the disc reports"
                      " no position", lba, playing, track);
            else
                trace2("no decode: lba %d, playing %d, track %d"
                       " - still", lba, playing, track);
        }
        return TRUE;
    }
    g_nodecode_said = 0;        /* decoding again - the next stall speaks */

    /*
     * DECODE ALWAYS, REPAINT ONLY IF IT CAN BE SEEN - 2026-09-28.
     *
     * THE DECODE CANNOT BE SKIPPED. CD+G is INCREMENTAL: the screen
     * at sector N depends on every packet before it, so a gap has
     * to be walked rather than jumped. Skipping it while hidden
     * would mean pressing Show replayed the whole track to catch
     * up - `decode_to()' would do it correctly, but in one call.
     *
     * THE REPAINT CAN. `repaint()' walks 64,800 pixels and issues a
     * filled rectangle per run; on a P1 that is the expensive half,
     * and drawing into a widget the user has hidden buys nothing.
     * `cdg_set_hidden()' moves the widget out of the way and this
     * is the matching saving.
     */
    if (g_after_change & CHG_DECODE)
        trace("first decode after the change starts: to lba %d, track %d",
              lba, track);
    decode_to(lba, track);
    if (g_after_change & CHG_DECODE) {
        g_after_change &= ~CHG_DECODE;
        trace("first decode after the change done: at lba %d", g_sub_lba);
    }
    /* THE PICTURE CAUGHT UP - decoding is capped per poll, so the screen
     * can stay black over several; this says when it stopped being. */
    if ((g_after_change & CHG_CAUGHT) && g_sub_lba >= lba) {
        g_after_change &= ~CHG_CAUGHT;
        trace("decode caught up with play at lba %d", lba);
    }
    if (!g_hidden) {
        int first = (g_after_change & CHG_REPAINT) != 0;

        repaint();
        if (first) {
            g_after_change &= ~CHG_REPAINT;
            trace("first repaint after the change done");
        }
    }
    return TRUE;                /* keep the timeout */
}

/*
 * FIND THE DISC AND FOLLOW IT, SILENTLY - split out of
 * transport_drive() 2026-09-27 so cdg_set_active() can call it.
 *
 * NO `say()' HERE. A transport BUTTON with no disc deserves a
 * notice; merely opening the page does not, and calling the
 * reporting version on activation would nag anyone who visits the
 * page without a disc attached.
 *
 * -1 when there is no drive with audio.
 */
/*
 * THE DRIVE THE VIEWER ITSELF SET PLAYING - 2026-10-03, for
 * [CDG Viewer] StopOnSwap. `g_transport' follows the drive's real
 * state, whoever started it, so it cannot say whether KsCD or this
 * page pressed play; this can. Set by every play and resume below,
 * cleared by Stop and by the swap that stops it.
 */
static int g_played_drive = -1;

/* 1 once "no drive with audio" has been said for this absence. */
static int g_nodrive_said;

static int
viewer_play(int drv, int track, int secs)
{
    int r = vlhe_cd_play(drv, track, secs);

    if (r == 0)
        g_played_drive = drv;
    return r;
}

static int
viewer_resume(int drv)
{
    int r = vlhe_cd_resume(drv);

    if (r == 0)
        g_played_drive = drv;
    return r;
}

/*
 * THE VIEWER MOVED OFF A DRIVE IT WAS PLAYING - stop it, if the user
 * asked for that (the CD page's Options; the user, 2026-10-03: "Add an
 * option that stops the disc the viewer was playing on swap"). Off by
 * default: two drives are two players, and before this option a swap
 * left the old disc playing. Only a disc the VIEWER started - KsCD's
 * plays on. `now' is the drive the viewer moved to, -1 for none.
 */
static void
swap_stop(int now)
{
    int old = g_played_drive;

    if (old < 0 || old == now || !vlhe_cdg_stop_on_swap())
        return;
    g_played_drive = -1;
    trace("viewer moved from drive %d to %d - stopping drive %d",
          old, now, old);
    (void) vlhe_cd_stop(old);
}

void
cdg_drive_chosen(void)
{
    swap_stop(audio_drive());
}

static int
follow_disc(void)
{
    int d = audio_drive();

    /*
     * THE FIRST QUESTION, AND NOTHING HAS EVER ANSWERED IT.
     * `audio_drive()' wants a drive that is attached AND reports
     * audio_tracks > 0, and those counts come from a SECOND control
     * exchange (`vlhe_backend.c:1205', the `tracks' verb) that is
     * skipped entirely when the daemon did not answer `status'. So
     * -1 here means either "no disc" or "the daemon did not tell us
     * about it", and the page looks identical in both cases.
     */
    if (d < 0) {
        /*
         * SAID ONCE, WHEN IT BECOMES TRUE - 2026-10-03. This ran every
         * second while the drive was empty: 176 "no drive with audio"
         * blocks in one run, and a SECOND `status' request to vdiscd
         * each time, only to print the per-drive detail. Now the first
         * second says it (the detail at level 2) and the rest are quiet
         * until a disc appears and it can become true again.
         */
        if (!g_nodrive_said) {
            g_nodrive_said = 1;
            if (vlhe_tracing() >= 1) {
                struct vlhe_drive dv[VLHE_MAX_DRIVE];
                int n = vlhe_drives(dv, VLHE_MAX_DRIVE);
                int k;

                trace("no drive with audio - %d drive(s) known", n);
                for (k = 0; k < n; k++)
                    trace2("  drive %d: attached=%d tracks=%d audio=%d"
                           " image=`%s'", dv[k].index, dv[k].attached,
                           dv[k].tracks, dv[k].audio_tracks, dv[k].image);
            }
        }

        /*
         * AN EJECT IS A DISC CHANGE, AND THIS RETURNED BEFORE
         * NOTICING ONE - 2026-09-30.
         *
         * The disc-change check below is the only caller of
         * `image_follow()', and this early return jumps over it -
         * so a disc LEAVING was seen, logged, and otherwise
         * ignored. The TOC, the track dropdown, the picture and
         * the transport state all kept the ejected disc's
         * contents while the status line correctly said there was
         * no disc.
         *
         * THE USER FOUND BOTH HALVES. Ejecting left tracks in the
         * dropdown that could be selected and would not play; and
         * REATTACHING THE SAME IMAGE was invisible too, because
         * `d == g_toc_drive' and the path had not changed either,
         * so the check below could never fire.
         *
         * THE LOG SHOWS IT: `drive 0: attached=0 tracks=0 audio=0
         * image=`''' on every poll, then `no decode: lba 0,
         * playing 0, track 1' after the reattach - the drive
         * answering, nothing playing, and the GUI still saying
         * "Playing" (tests/logs/2026-09-30-cdg-eject-stale-state).
         *
         * FORGETTING THE DISC HERE FIXES BOTH: clearing
         * `g_toc_drive' means the next attach differs by DRIVE,
         * so it reloads whether or not the path is the same one.
         *
         * THE PICTURE IS LEFT ALONE, at the user's request - a
         * held frame is useful for comparing against a reference
         * player, and a real player has nothing to show either
         * way. `image_follow(-1)' would clear it; this does the
         * rest of what that does, by hand.
         */
        if (g_toc_drive >= 0) {
            trace("the disc has gone - forgetting drive %d `%s'",
                  g_toc_drive, g_toc_image);
            swap_stop(-1);
            if (g_img_open) {
                vdisc_image_close(&g_img);
                g_img_open = 0;
            }
            g_sub_lba = -1;
            g_ntoc = 0;
            g_toc_drive = -1;
            g_toc_image[0] = '\0';
            g_disc_tracks = 0;
            g_disc_cur = 1;
            memset(&g_disc, 0, sizeof g_disc);
            tracks_load(0, NULL);
            transport_set(TRANSPORT_STOPPED);
            time_show(-1, 0, 0);
            /*
             * AND THE LINE AND THE PICTURE - found on 86Box 2026-10-01
             * when an eject while playing left both showing the old
             * disc under a menu saying "(no disc)". The line clears
             * always; the picture by the option (g_clear_on_eject).
             */
            info_load(0, NULL);
            if (g_clear_on_eject) {
                cdg_reset(&g_screen);
                if (!g_hidden)
                    repaint();
            }
        }
        return -1;
    }

    g_nodrive_said = 0;         /* a disc again - the next absence speaks */

    /*
     * READ THE TOC ONCE PER DISC, not per press. Nothing about a
     * layout changes while a disc is in the drive, and the seek
     * path needs it on every button. Keyed on the drive so an
     * eject-and-attach elsewhere is picked up.
     */
    /*
     * AND ON THE IMAGE PATH, NOT THE DRIVE INDEX ALONE - 2026-09-27.
     *
     * KEYING ON THE INDEX MISSED A DISC CHANGE. Swapping the image
     * in drive 0 leaves the index 0, so this re-read nothing: the
     * user attached a new disc and *"it didnt update. It still had
     * that buffered play stopping and playing. Restarting the gui
     * fixed the track info and everything."*
     *
     * THE PATH IS THE DISC'S IDENTITY here. Comparing it costs one
     * `image' round trip per press, which `image_follow()' was
     * making anyway, and catches an eject-and-attach into the SAME
     * drive - the ordinary case on a one-drive machine, and the one
     * the index can never see.
     */
    {
        char now[VLHE_PATH_MAX];

        if (vlhe_drive_image(d, now, sizeof now) != 0)
            now[0] = '\0';

        if (d != g_toc_drive || strcmp(now, g_toc_image) != 0) {
            /* BRACKETED, so the timings show what a switch costs. */
            trace("disc change: drive %d `%s' -> drive %d `%s'",
                  g_toc_drive, g_toc_image, d, now);
            if (d != g_toc_drive)
                swap_stop(d);
            g_ntoc = vlhe_cd_toc(d, g_toc, CDG_MAX_TOC);
            if (g_ntoc < 0)
                g_ntoc = 0;
            trace("toc read - %d entr%s", g_ntoc, g_ntoc == 1 ? "y" : "ies");
            g_toc_drive = d;
            strncpy(g_toc_image, now, sizeof g_toc_image - 1);
            g_toc_image[sizeof g_toc_image - 1] = '\0';
            image_follow(d);    /* the same question: what disc is this */
            trace("disc change done - drive %d", d);
            watch_arm();
        }
    }
    return d;
}

/* Report the same way for every transport verb, so a machine with
 * no disc says one thing rather than four. */
static int
transport_drive(void)
{
    int d = follow_disc();

    if (d < 0)
        say(STR_CDG_MSG_NO_DISC_AUDIO_ATTACH);
    return d;
}

/*
 * PLAY / PAUSE / RESUME - ONE BUTTON, AS EVERY CD PLAYER HAS.
 *
 * Asked for 2026-09-29, and the reason is worth keeping: comparing
 * our picture against a reference player means CAPTURING A FRAME,
 * and without a pause there is nothing to capture. It is also
 * simply how KsCD and every physical player behave.
 *
 * THE DAEMON ALREADY DOES ALL OF IT. `vlhe_cd_pause()' and
 * `vlhe_cd_resume()' have existed the whole time - CMD_PAUSE and
 * CMD_RESUME in vdiscd, CDROMPAUSE and CDROMRESUME in the module,
 * with CDROM_AUDIO_PAUSED tracked as its own state. Only the GUI
 * never called them: the play button issued
 * `vlhe_cd_play(drv, g_disc_cur, 0)' every time, which is "restart
 * the selected track from zero".
 *
 * AND RESUME IS NOT A SEEK. The obvious-looking alternative - save
 * the position and re-issue play at that offset - is how you would
 * do it if `vlhe_cd_resume()' did not exist, and it would be
 * wrong: `vlhe_backend.h' records that a `vlhe_cd_seek()' was
 * written and REMOVED, because seeking here means
 * `vlhe_cd_play(drive, track, secs + delta)' against a TOC the
 * caller holds. The daemon keeps the true position across a pause,
 * so resume needs no arithmetic at all.
 *
 * WHY THE STATE IS TRACKED HERE rather than read back:
 * `vlhe_cd_position()' reports `playing' as
 * `audiostatus == CDROM_AUDIO_PLAY', so a paused drive is
 * indistinguishable from a stopped one through that call. The
 * module does distinguish them; surfacing that would mean widening
 * the backend API for one caller.
 */

static void
transport_set(int state)
{
    if (g_transport == state)
        return;
    g_transport = state;

    /* THE BUTTON SHOWS WHAT IT WILL DO NEXT, which is the
     * convention every player follows: a pause glyph while
     * playing, a play glyph otherwise. */
    if (g_play_icon != NULL) {
        /* STORED PLUS ONE, because ICON_PLAY is 0 and
         * GINT_TO_POINTER(0) is NULL - which the expose handler
         * cannot tell from "no datum set". */
        gtk_object_set_data(GTK_OBJECT(g_play_icon), "icon",
                            GINT_TO_POINTER((state == TRANSPORT_PLAYING
                                             ? ICON_PAUSE : ICON_PLAY) + 1));
        gtk_widget_queue_draw(g_play_icon);
    }
}

static void on_play(GtkWidget *w, gpointer d)
{
    int drv = transport_drive();
    char msg[80];

    (void)w; (void)d;
    if (drv < 0)
        return;

    /*
     * PLAYING -> PAUSE, PAUSED -> RESUME, STOPPED -> PLAY.
     * A failure leaves the state alone, so the button keeps
     * offering the action that did not happen.
     */
    if (g_transport == TRANSPORT_PLAYING) {
        if (vlhe_cd_pause(drv) != 0) {
            say(STR_CDG_MSG_COULD_NOT_PAUSE);
            trace("pause on drive %d FAILED - %s", drv, strerror(errno));
            return;
        }
        transport_set(TRANSPORT_PAUSED);
        say(STR_CDG_MSG_PAUSED);
        return;
    }

    if (g_transport == TRANSPORT_PAUSED) {
        if (viewer_resume(drv) != 0) {
            say(STR_CDG_MSG_COULD_NOT_RESUME);
            trace("resume on drive %d FAILED - %s", drv, strerror(errno));
            return;
        }
        transport_set(TRANSPORT_PLAYING);
        say(STR_CDG_MSG_PLAYING);
        return;
    }

    if (viewer_play(drv, g_disc_cur, 0) != 0) {
        /*
         * SAY WHICH, NOT JUST THAT - 2026-09-28. "Could not start
         * playback" on its own cost a target run to narrow, because
         * `vlhe_cd_play()' has three failure paths and none of them
         * said anything. It sets errno now; this prints it.
         *
         * ENOMEDIUM is the one worth a sentence: it means the TOC
         * did not come back, so the daemon or the disc is the
         * problem rather than the track.
         */
        sprintf(msg, FMT_CDG_COULD_NOT_START_PLAYBACK,
                errno == ENOMEDIUM ? STR_CDG_TEXT_NO_DISC_DAEMON
                                   : strerror(errno));
        say(msg);
        trace("play track %d on drive %d FAILED - %s",
              g_disc_cur, drv, strerror(errno));
        return;
    }
    transport_set(TRANSPORT_PLAYING);
    sprintf(msg, FMT_CDG_PLAYING_TRACK, g_disc_cur);
    say(msg);
}

static void on_stop(GtkWidget *w, gpointer d)
{
    int drv = audio_drive();

    (void)w; (void)d;
    if (drv >= 0 && vlhe_cd_stop(drv) != 0)
        say(STR_CDG_MSG_COULD_NOT_STOP_DRIVE);   /* design/47 section 5 */
    if (drv == g_played_drive)
        g_played_drive = -1;
    transport_set(TRANSPORT_STOPPED);
    /* THE PICTURE GOES WITH IT, whether or not a drive answered -
     * a stopped player showing the last frame would be claiming
     * something it is not doing. */
    say(STR_CDG_MSG_STOP);
    cdg_reset(&g_screen);
    repaint();
}

/*
 * SEEK - 30 SECONDS, AND IT IS PLAY WITH AN OFFSET.
 *
 * KsCD HAS NO SEEK COMMAND EITHER and this is its mechanism
 * (`kscd.cpp:786'): take the track-relative position, add the
 * delta, and re-issue PLAY from there. A period player had no other
 * way - the kernel path is CDROMPLAYMSF, which takes a start
 * address - and neither do we.
 *
 * 30 SECONDS BOTH WAYS, which is KsCD's ("30 Secs Forward", "30
 * Secs Backward", `:565'). This page said "Back 10 seconds" and
 * "Forward 30" until 2026-09-26, asymmetric with no precedent.
 *
 * THE POSITION IS COMPUTED HERE, from a TOC we hold and a frame the
 * KERNEL gives - exactly `cdrom.c:431',
 * `(cur_frame - trk[n].start) / 75'. No daemon is asked where
 * playback has reached, which is just as well: it does not know.
 *
 * BACKWARD CLAMPS TO THE TRACK START (`:804'); forward past the end
 * is refused by the daemon rather than clamped (`:787'), because
 * running into the next track is not what the button means.
 *
 * AND ONLY WHILE PLAYING, as KsCD guards it - a seek on a stopped
 * disc has nothing to seek from.
 */
static void
seek_by(int delta)
{
    int drv = transport_drive();
    int lba, track = 0, playing = 0, secs, i;

    if (drv < 0)
        return;

    lba = vlhe_cd_position(drv, &track, &playing);
    if (lba < 0 || !playing) {
        say(STR_CDG_MSG_NOTHING_PLAYING);
        return;
    }

    /* WHERE THIS TRACK STARTS, from the TOC we read at attach. */
    for (i = 0; i < g_ntoc; i++)
        if (g_toc[i].num == track)
            break;
    if (i >= g_ntoc) {
        say(STR_CDG_MSG_CANNOT_TELL_WHERE_TRACK);
        return;
    }

    secs = (lba - g_toc[i].start) / 75 + delta;
    if (secs < 0)
        secs = 0;                       /* clamp, as KsCD does */

    if (viewer_play(drv, track, secs) != 0) {
        /* THE DAEMON REFUSED, which for a forward seek means the
         * end of the track - say what happened rather than nothing. */
        say(delta > 0 ? STR_CDG_MSG_NEAR_END_OF_TRACK
                      : STR_CDG_MSG_COULD_NOT_SEEK);
        return;
    }
    /* SEEKING FROM PAUSED STARTS PLAYING, so the state must follow
     * or the play button would offer to resume something already
     * running. Every vlhe_cd_play() here does this. */
    transport_set(TRANSPORT_PLAYING);
    {
        char msg[64];
        sprintf(msg, FMT_CDG_SECONDS_INTO_TRACK, secs, track);
        say(msg);
    }
}

static void on_rw(GtkWidget *w, gpointer d)
{ (void)w; (void)d; seek_by(-30); }

static void on_ff(GtkWidget *w, gpointer d)
{ (void)w; (void)d; seek_by(30); }

/* PREVIOUS - the mirror of on_next(): from the PLAYING position,
 * and it wraps to the last track rather than sticking at 1. */
static void on_prev(GtkWidget *w, gpointer d)
{
    int drv, lba, track = 0, playing = 0, prev;

    (void)w; (void)d;

    drv = transport_drive();
    if (drv < 0) {
        say(STR_CDG_MSG_NO_DISC_PLAY);    /* design/47 G5: not in silence */
        return;
    }

    lba = vlhe_cd_position(drv, &track, &playing);
    if (lba < 0 || track <= 0)
        track = g_disc_cur;

    prev = (track <= 1) ? (g_disc_tracks > 0 ? g_disc_tracks : 1)
                        : track - 1;

    if (viewer_play(drv, prev, 0) != 0) {
        char msg[80];

        sprintf(msg, FMT_CDG_COULD_NOT_PLAY_TRACK, prev);
        say(msg);
    } else {
        char msg[80];

        transport_set(TRANSPORT_PLAYING);
        g_disc_cur = prev;
        tracks_select(prev);
        info_load(g_disc_cur, &g_disc);
        cdg_reset(&g_screen);
        g_sub_lba = -1;
        repaint();
        sprintf(msg, FMT_CDG_TRACK, prev);
        say(msg);
    }
}

/*
 * NEXT - FROM THE PLAYING POSITION, AND IT WRAPS. KsCD's
 * `nextClicked()':
 *
 *     if (cur_track == cur_ntracks)
 *         cur_track = 0;
 *     play_cd (cur_track + 1, 0, cur_ntracks + 1);
 *
 * TWO THINGS COPIED, both confirmed by the user on target
 * 2026-09-28 (*"the drop down does update to the current track
 * playing when hit next or back"*):
 *
 * IT DOES NOT READ THE DROPDOWN. `cur_track' is where the disc IS,
 * and the dropdown is an OUTPUT of the transport - `tracks_select()'
 * follows it, never the reverse. This used to advance `g_disc_cur',
 * which the dropdown ALSO sets, so selecting track 5 while track 2
 * played made Next jump to 6 instead of 3.
 *
 * AND IT WRAPS at the last track rather than sticking there, which
 * is what the `cur_track = 0' line above does.
 */
static void on_next(GtkWidget *w, gpointer d)
{
    int drv, lba, track = 0, playing = 0, next;

    (void)w; (void)d;

    drv = transport_drive();
    if (drv < 0) {
        say(STR_CDG_MSG_NO_DISC_PLAY);    /* design/47 G5: not in silence */
        return;
    }

    /* WHERE THE DISC IS, not where the list is pointing. Falls back
     * to the selection only when nothing is playing and the drive
     * cannot say. */
    lba = vlhe_cd_position(drv, &track, &playing);
    if (lba < 0 || track <= 0)
        track = g_disc_cur;

    next = (track >= g_disc_tracks) ? 1 : track + 1;

    if (viewer_play(drv, next, 0) != 0) {
        char msg[80];

        sprintf(msg, FMT_CDG_COULD_NOT_PLAY_TRACK, next);
        say(msg);
    } else {
        char msg[80];

        transport_set(TRANSPORT_PLAYING);
        g_disc_cur = next;
        tracks_select(next);
        info_load(g_disc_cur, &g_disc);
        cdg_reset(&g_screen);
        g_sub_lba = -1;
        repaint();
        sprintf(msg, FMT_CDG_TRACK, next);
        say(msg);
    }
}

/* ------------------------------------------------------------------ */

/*
 * RE-DERIVE WHETHER THE POLL SHOULD RUN.
 *
 * TWO INDEPENDENT REASONS TO BE LIVE and either is enough: the page
 * is selected, or the picture is in its own window. Called from
 * BOTH places that can change either - the shell's page switch and
 * detach/attach - because a reattach while on another page would
 * otherwise leave `g_active' stuck on, polling a page nobody is
 * looking at.
 */
static void
cdg_refresh_active(void)
{
    g_active = (g_page_selected || g_float != NULL) ? 1 : 0;
}

void
cdg_set_active(int on)
{
    /*
     * A DETACHED WINDOW STAYS LIVE - 2026-09-28, the user's
     * question: what does the viewer do when it is not the
     * selected page?
     *
     * THE SHELL CALLS THIS WITH `which == MOD_CDG', so switching to
     * Sound Settings used to set `g_active = 0' and `on_poll()'
     * returned at its first line. That is RIGHT for the attached
     * page - nothing is on screen, so decoding is waste - and WRONG
     * for a DETACHED one, which is a separate top-level window
     * still visible while the user works elsewhere. It froze
     * mid-song with the music playing on, which is the opposite of
     * what detaching is for.
     *
     * SO THE FLOAT OVERRIDES THE SIDEBAR. `g_float' is the detached
     * window and is NULL when the picture is in the page, so the
     * two cannot disagree.
     *
     * COMING BACK NEEDS NOTHING: `decode_to()' keeps `g_sub_lba'
     * across the gap and walks forward from it, and its budget is
     * 4096 sectors PER CALL - about 55 seconds of disc - so even a
     * long absence is caught up in well under a second of polling.
     */
    g_page_selected = on ? 1 : 0;
    cdg_refresh_active();

    /*
     * AND FIND THE DISC - 2026-09-27, the user: "ensure the page is
     * refreshed. that was an issue with some of the pages before".
     *
     * THE PAGE USED TO LEARN ABOUT A DISC ONLY FROM A TRANSPORT
     * BUTTON. `g_toc_drive' is set in exactly one place -
     * `transport_drive()' - so opening this page with a disc already
     * attached left it at -1, and then:
     *
     *   - `on_poll()' returns early on `g_toc_drive < 0', so nothing
     *     decoded and nothing repainted: A BLACK WINDOW
     *   - `image_follow()' had never run, so no CD-TEXT, no CDDB
     *     lookup and no `g_disc_tracks': AN EMPTY TRACK LIST
     *
     * Both of the symptoms reported on 2026-09-27, from one cause.
     * Pressing Play fixed it by accident, which is why the transport
     * appeared to work while the page looked dead.
     *
     * CHEAP AND IDEMPOTENT: `transport_drive()' re-reads the TOC only
     * when the drive INDEX changes, so a second activation with the
     * same disc does nothing but one `vlhe_drives()' call. It is not
     * called on deactivation - there is nothing to refresh on a page
     * going away.
     */
    if (g_active) {
        trace("page shown - looking for a disc");
        watch_arm();            /* D60: five seconds of layout tracing */
        (void) follow_disc();   /* silent to the USER: see follow_disc() */

        /* AND DRAW WHAT WE ALREADY HAVE. Coming back to the page,
         * `g_screen' still holds the last decoded frame but the
         * backing store may not - and the poll only repaints when
         * something is PLAYING, so a paused disc would show an
         * empty picture until the user pressed play. */
        if (!g_hidden)
            repaint();
    }
}

/* ------------------------------------------------------------------ */
/* Detaching, by button                                               */
/* ------------------------------------------------------------------ */

/*
 * A BUTTON, NOT A DRAG HANDLE - the user's finding, 2026-09-20: the
 * GtkHandleBox worked, and "is there away to have a button to
 * attach/detach".
 *
 * GtkHandleBox CANNOT BE DRIVEN FROM CODE. Its `child_detached' is a
 * read-only status bit (gtkhandlebox.h:65) and the header exposes
 * only shadow type, handle position and snap edge - the float is
 * raised by dragging and by nothing else. So a button means doing the
 * reparenting ourselves, which is the gtk_widget_reparent() route
 * design/34 section 6d listed as the alternative.
 *
 * THE WHOLE VBOX MOVES, viewer and transport together, which was the
 * user's call when the HandleBox went in and has not changed.
 *
 * `g_slot' IS AN EMPTY BOX THAT STAYS ON THE PAGE. Reparenting needs
 * somewhere to put the child back, and holding a pointer to the page
 * container is not enough - the shell may destroy and rebuild a page.
 * An empty GtkVBox that only ever holds the viewer is the simplest
 * thing that survives.
 */

static void detach_set(int detached);
static void cdg_set_hidden(int hidden);

/*
 * SHRINK THE FLOAT TO ITS CONTENTS.
 *
 * A GtkWindow keeps whatever size it was given, so after a zoom
 * change or a hide it is left with dead space. Setting the size
 * request to the child's own and then resizing to 1x1 makes GTK fall
 * back to the natural size - the standard way to say "as small as
 * you can be" in 1.2, where there is no gtk_window_resize_to_fit().
 */
static void
cdg_fit_float(void)
{
    if (g_float == NULL)
        return;
    /*
     * GTK 1.2 HAS NO gtk_window_resize() - that is 2.x. The idiom
     * here is set_policy with `auto_shrink' TRUE, which makes the
     * window follow its child's requisition downward as well as up,
     * plus a usize of -1,-1 to clear any override that would pin it
     * larger. gtkwindow.h:117 spells out the difference between
     * set_usize and set_default_size for exactly this case.
     */
    gtk_window_set_policy(GTK_WINDOW(g_float), FALSE, TRUE, TRUE);
    gtk_widget_set_usize(g_float, -1, -1);
    gtk_widget_queue_resize(g_float);
}

/*
 * HIDE THE PICTURE - a stand-alone CD player, the user's idea.
 *
 * DETACHED ONLY, and the button is hidden otherwise: a MOD_VIEW page
 * exists to show something, so hiding the picture there would leave
 * an empty pane and no obvious way back. Floating, the transport and
 * the track list are a perfectly good small player and the graphics
 * are the optional part.
 */
static void
cdg_set_hidden(int hidden)
{
    g_hidden = hidden ? 1 : 0;

    if (g_picture != NULL) {
        if (g_hidden)
            gtk_widget_hide(g_picture);
        else
            gtk_widget_show(g_picture);
    }

    if (g_hide_btn != NULL)
        gtk_label_set_text(GTK_LABEL(GTK_BIN(g_hide_btn)->child),
                           g_hidden ? STR_CDG_BTN_SHOW : STR_CDG_BTN_HIDE);

    cdg_fit_float();

    /*
     * SHOWING IT REPAINTS AT ONCE. The poll skips `repaint()' while
     * hidden (it still DECODES, so `g_screen' is current), which
     * means nothing has drawn into the backing store for as long as
     * the picture was away. Without this the widget would come back
     * holding whatever was there when it was hidden and stay that
     * way until the next tick - a quarter second of a stale frame,
     * and longer if playback is stopped, because the poll returns
     * early when nothing is playing.
     */
    if (!g_hidden)
        repaint();

    if (g_report)
        g_report(g_hidden ? STR_CDG_MSG_GRAPHICS_HIDDEN
                          : STR_CDG_MSG_GRAPHICS_SHOWN);
}

/*
 * THE TRACK CHANGED, so everything that describes a track has to
 * follow it.
 *
 * NOTHING CALLED info_load() ON A SELECTION until the user asked
 * whether the text was meant to change - it was built once and then
 * kept whatever it was given, which looked identical to a stub. The
 * dropdown is the only control that selects a track, so it is the
 * only place this can hang.
 *
 * THE TRACK NUMBER COMES FROM THE TEXT, not from a row index: the
 * list is built "%02d: ..." and a disc need not start at track 1, so
 * position and number are different things.
 */
/*
 * A GtkOptionMenu ITEM, NOT A GtkCombo ENTRY - changed 2026-09-28 at
 * the user's instruction: *"can you change it to match the ones that
 * the sound options uses for the play through"*.
 *
 * THE COMBO WAS WHY THE LIST LOOKED EMPTY. `GtkCombo' is an entry
 * field with a popdown beside it, and the ENTRY is what shows - so a
 * page that kept writing the playing track into that entry
 * presented as a box with one item in it. The user found it:
 * *"The drop down was working all along but it was because it was
 * one you could type in vs the other type"*.
 *
 * `GtkOptionMenu' has no entry. The widget IS the list, the current
 * item is shown with `gtk_option_menu_set_history()', and there is
 * nothing to type into - which is what `vlhe_mod_sound.c' uses for
 * "Play through:" and what this should have been.
 *
 * THE TRACK NUMBER IS THE ROW PLUS ONE, since the picker lists the
 * tracks in order and row 0 is track 1. The option menu this
 * replaced carried the number on each item as user data, because a
 * menu has no order to read back; a list row does.
 */
static void
on_track_picked(gint row, const gchar *text, gpointer d)
{
    int n = row + 1;

    (void)text; (void)d;

    if (g_tracks == NULL || n <= 0 || n >= CDTEXT_MAX_TRACKS)
        return;

    /*
     * DO NOT ACT ON OUR OWN UPDATE. `tracks_select()' sets this
     * widget from the PLAYING position four times a second, and
     * GTK fires "changed" for that exactly as it does for a user
     * pick. Without this guard the poll would call play() on every
     * tick and the disc would restart the current track forever.
     */
    if (g_selecting || n == g_disc_cur)
        return;

    g_disc_cur = n;
    info_load(g_disc_cur, &g_disc);

    /*
     * AND PLAY IT - 2026-09-28. KsCD TREATS A SELECTION AS A PLAY
     * COMMAND, and the user confirmed it on the target the same
     * day: *"kscd does treat it as a play command"*.
     *
     * `trackSelected(int trk)' (`kscd.cpp') is four lines:
     *
     *     cur_track = trk + 1;
     *     play_cd( cur_track, 0, cur_ntracks + 1 );
     *
     * NOTE THE THIRD ARGUMENT - `cur_ntracks + 1', the LEAD-OUT.
     * Picking track 3 plays 3, 4, 5 ... to the end of the disc
     * rather than stopping at the end of 3. We do the same by
     * passing 0 for the length, which `vlhe_cd_play()' reads as
     * "to the lead-out".
     *
     * THE COMMENT THIS REPLACES SAID "nothing is wired to a disc
     * yet (design/34 step 3)". Step 3 is built; the handler was
     * never brought forward with it, so choosing a track moved the
     * labels and the music carried on regardless.
     */
    {
        int drv = transport_drive();

        if (drv >= 0) {
            char msg[64];

            cdg_reset(&g_screen);
            g_sub_lba = -1;             /* decode again from the start */
            repaint();

            if (viewer_play(drv, g_disc_cur, 0) == 0) {
                transport_set(TRANSPORT_PLAYING);
                sprintf(msg, FMT_CDG_PLAYING_TRACK, g_disc_cur);
                say(msg);
            } else {
                sprintf(msg, FMT_CDG_COULD_NOT_PLAY_TRACK, g_disc_cur);
                say(msg);
            }
        } else {
            /* No disc: the labels still moved, which is honest -
             * the user picked a track and we show its name. */
            cdg_reset(&g_screen);
            repaint();
        }
    }
}

static void
on_hide_clicked(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    cdg_set_hidden(!g_hidden);
}

static gint
on_float_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;(void)e;(void)d;

    /* CLOSING THE FLOAT RE-ATTACHES rather than destroying the
     * viewer. A window manager close button that left the page empty
     * would be a trap. */
    detach_set(0);
    return TRUE;                /* we handled it; do not destroy */
}

/*
 * WHERE ATTACH AND THE TRACK LIST SIT - the user's "Compact when
 * detached", 2026-10-01: detached with the option on, both move to a
 * second row under the player controls, so the floating window is as
 * narrow as the transport and one row taller; attached, or with the
 * option off, they are at the end of the one row as before.
 * gtk_widget_reparent() keeps the widgets and their signals; only the
 * box changes.
 */
static void
layout_rows(void)
{
    int compact = (g_float != NULL && g_compact_detached);

    if (g_row1 == NULL || g_row2 == NULL
        || g_detach_btn == NULL || g_tracks == NULL)
        return;

    if (compact && g_detach_btn->parent != g_row2) {
        gtk_widget_reparent(g_detach_btn, g_row2);
        gtk_widget_reparent(g_tracks, g_row2);
        gtk_box_set_child_packing(GTK_BOX(g_row2), g_tracks,
                                  TRUE, TRUE, 0, GTK_PACK_END);
        gtk_widget_show(g_row2);
    } else if (!compact && g_detach_btn->parent != g_row1) {
        gtk_widget_reparent(g_detach_btn, g_row1);
        gtk_widget_reparent(g_tracks, g_row1);
        gtk_box_set_child_packing(GTK_BOX(g_row1), g_tracks,
                                  FALSE, FALSE, 0, GTK_PACK_END);
        gtk_widget_hide(g_row2);
    }
    if (g_float != NULL)
        cdg_fit_float();
}

static void
detach_set(int detached)
{
    if (g_vbox == NULL || g_slot == NULL)
        return;

    if (detached && g_float == NULL) {
        g_float = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(g_float), STR_MOD_CDG);
        gtk_signal_connect(GTK_OBJECT(g_float), "delete_event",
                           GTK_SIGNAL_FUNC(on_float_delete), NULL);

        gtk_widget_reparent(g_vbox, g_float);
        gtk_widget_show(g_float);
        layout_rows();          /* compact, if the option says so */

        /*
         * SIZE THE WINDOW TO WHAT IS IN IT. Without this the float
         * opens at whatever the page pane was and a 1x picture sits
         * in a 2x-sized frame with dead space around it - the user
         * caught exactly that. gtk_window_set_default_size() is
         * wrong here (it only applies before the first map), so this
         * resizes to the child's own request.
         */
        cdg_fit_float();

        /* The page now holds an empty slot; let it shrink rather
         * than keeping the space the viewer used. */
        gtk_widget_queue_resize(g_slot);
        if (g_slot->parent != NULL)
            gtk_widget_queue_resize(g_slot->parent);

        if (g_detach_btn != NULL)
            gtk_label_set_text(
                GTK_LABEL(GTK_BIN(g_detach_btn)->child), STR_CDG_BTN_ATTACH);
        /* HIDE ONLY MAKES SENSE DETACHED - hiding the picture on the
         * page would leave a MOD_VIEW page with nothing to view. */
        if (g_hide_btn != NULL)
            gtk_widget_show(g_hide_btn);
        if (g_report)
            g_report(STR_CDG_MSG_VIEWER_DETACHED);

        /* THE FLOAT IS ITS OWN REASON TO POLL, even if the sidebar
         * is on another page. See cdg_refresh_active(). */
        cdg_refresh_active();

    } else if (!detached && g_float != NULL) {
        gtk_widget_reparent(g_vbox, g_slot);
        gtk_widget_destroy(g_float);
        g_float = NULL;
        layout_rows();          /* back to the one row */

        /* AND IT STOPS BEING ONE. Reattaching while the sidebar is
         * elsewhere must stop the poll, or it decodes into a page
         * nobody is looking at. */
        cdg_refresh_active();

        /*
         * TELL THE PAGE ITS CONTENT CAME BACK.
         *
         * The shell wraps the pane in a GtkScrolledWindow
         * (vlhe_cc.c's g_scroll) so the window can go below 800x600.
         * Reparenting a child OUT leaves that scrolled window sized
         * for content that is no longer there, and reparenting it
         * back does not re-ask: the scrollbars stayed until the user
         * resized the window by hand, which they had to do to clear
         * them - "I had to full screen and window again for the
         * scroll bars to hide again after attaching".
         *
         * queue_resize on the slot propagates up through the
         * scrolled window, which then recomputes whether it needs
         * bars at all.
         */
        gtk_widget_queue_resize(g_slot);
        if (g_slot->parent != NULL)
            gtk_widget_queue_resize(g_slot->parent);

        /* RE-ATTACHING UNHIDES. A hidden picture back on the page
         * would be a MOD_VIEW page showing nothing, with the control
         * that restores it now invisible. */
        if (g_hidden)
            cdg_set_hidden(0);

        if (g_detach_btn != NULL)
            gtk_label_set_text(
                GTK_LABEL(GTK_BIN(g_detach_btn)->child), STR_CDG_BTN_DETACH);
        if (g_hide_btn != NULL)
            gtk_widget_hide(g_hide_btn);
        if (g_report)
            g_report(STR_CDG_MSG_VIEWER_REATTACHED);
    }
}

static void
on_detach_clicked(GtkWidget *w, gpointer d)
{
    (void)w;(void)d;
    detach_set(g_float == NULL);
}

/* ------------------------------------------------------------------ */
/* The track list and the information line                            */
/* ------------------------------------------------------------------ */

/*
 * FILL THE DROPDOWN FROM THE TOC, ANNOTATED WITH CD-TEXT.
 *
 * KsCD's shape (kscd.cpp:1295-1299): it fills from the title list,
 * then PADS TO THE TRACK COUNT with a placeholder. The count comes
 * from the drive and the text may run out, which is exactly the case
 * a disc with no CD-TEXT presents.
 *
 * Its placeholder is "%02d: <Unknown>", and the commented-out line
 * below it in that file shows they moved AWAY from "Track %02d" - a
 * numbered slot that shows its own emptiness beat one that looks
 * filled.
 */
static void
tracks_load(int n_tracks, const struct cdtext *ct)
{
    /* CDTEXT_MAX_TRACKS is 100 (index 0 unused), so 99 rows. */
    static gchar *rows[CDTEXT_MAX_TRACKS];
    static char   text[CDTEXT_MAX_TRACKS][160];
    int i, n = 0;

    if (g_tracks == NULL)
        return;

    if (n_tracks <= 0) {
        /* ONE ROW SAYING SO, rather than an empty list - an empty
         * picker draws a bare frame and reads as broken. */
        strcpy(text[0], STR_CDG_TEXT_NO_DISC_LIST);
        rows[0] = text[0];
        g_selecting = 1;
        vlhe_picker_set_items(g_tracks, rows, 1);
        g_selecting = 0;
        return;
    }

    for (i = 1; i <= n_tracks && i < CDTEXT_MAX_TRACKS; i++) {
        const char *title = (ct != NULL) ? ct->track[i].title : "";
        const char *who   = (ct != NULL) ? ct->track[i].performer : "";

        if (title[0] == '\0') {
            sprintf(text[n], FMT_CDG_TRACK_UNKNOWN, i);
        } else if (g_track_format == 1 && who[0] != '\0') {
            /* ARTIST AND TITLE - for a compilation, where every track
             * has a different performer. CDDB could not express that
             * at all; CD-TEXT can. */
            sprintf(text[n], "%02d: %.60s - %.60s", i, who, title);
        } else {
            sprintf(text[n], "%02d: %.120s", i, title);
        }
        rows[n] = text[n];
        n++;
    }

    /*
     * THE ROW INDEX IS THE TRACK NUMBER MINUS ONE, and that is the
     * whole of the mapping now - where the option menu carried the
     * track number on each item as user data, because a menu has no
     * inherent order to read back. A list row does.
     */
    g_selecting = 1;
    vlhe_picker_set_items(g_tracks, rows, n);
    if (g_disc_cur >= 1 && g_disc_cur <= n)
        vlhe_picker_select(g_tracks, g_disc_cur - 1);
    g_selecting = 0;
}

/*
 * POINT THE DROPDOWN AT A TRACK - 2026-09-27, so it follows what is
 * PLAYING rather than only what was last clicked.
 *
 * KsCD's `songListCB->setCurrentItem(cur_track - 1)' (`kscd.cpp:1304'
 * and `:1309'), which it calls on every status update.
 *
 * THROUGH THE LIST, NOT THE ENTRY. A GtkCombo's entry is a plain
 * text field; setting its text would show the right words and leave
 * the list's own selection elsewhere, so opening the dropdown would
 * disagree with the box above it. `gtk_list_select_item()' moves the
 * selection and GTK writes the entry from it.
 *
 * ONE-BASED IN, ZERO-BASED OUT - the disc's numbering is what every
 * other part of this page uses.
 */
static void
tracks_select(int track)
{
    if (g_tracks == NULL || track < 1 || track > g_disc_tracks)
        return;

    /* NO GUARD NEEDED NOW, and the guard is kept anyway.
     * vlhe_picker_select() does not notify - moving the selection
     * is not a user's choice - where gtk_option_menu_set_history()
     * fired "activate" exactly as a click did, and without the
     * guard the poll replayed the track. Belt and braces: the guard
     * costs nothing and the next person to touch this should not
     * have to know which of the two behaviours is current. */
    g_selecting = 1;
    vlhe_picker_select(g_tracks, track - 1);
    g_selecting = 0;
}

/*
 * BUILD THE INFORMATION LINE from the chosen fields, in their chosen
 * order.
 *
 * AN ABSENT FIELD IS SKIPPED, not left as a gap - the second fixture
 * in tests/fixtures/cdtext/ has an empty MESSAGE and an ARRANGER that
 * is a stray tab, so ragged data is the normal case rather than the
 * exception.
 */
static void
info_load(int track, const struct cdtext *ct)
{
    char out[512];
    char buf[128];
    char *tok;
    int   n = 0;

    if (g_info == NULL)
        return;

    out[0] = '\0';
    strncpy(buf, g_info_fields, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    for (tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        const char *v = NULL;
        char        tmp[32];

        if (ct != NULL && track > 0 && track < CDTEXT_MAX_TRACKS) {
            const struct cdtext_entry *e = &ct->track[track];
            const struct cdtext_entry *d = &ct->disc;

            if      (strcmp(tok, "title")      == 0)
                v = e->title[0]      ? e->title      : d->title;
            else if (strcmp(tok, "performer")  == 0)
                v = e->performer[0]  ? e->performer  : d->performer;
            else if (strcmp(tok, "songwriter") == 0)
                v = e->songwriter[0] ? e->songwriter : d->songwriter;
            else if (strcmp(tok, "composer")   == 0)
                v = e->composer[0]   ? e->composer   : d->composer;
            else if (strcmp(tok, "arranger")   == 0)
                v = e->arranger[0]   ? e->arranger   : d->arranger;
            else if (strcmp(tok, "message")    == 0)
                v = e->message[0]    ? e->message    : d->message;
        }

        if (strcmp(tok, "track") == 0 && track > 0) {
            sprintf(tmp, FMT_CDG_TRACK_N, track);
            v = tmp;
        } else if (strcmp(tok, "time") == 0) {
            /* THE TRACK'S LENGTH, from the TOC - design/47 G5. This
             * printed `--:--' as a stub; the elapsed time is the
             * clock widget's, which ticks, and this line is built
             * once per track, so the static fact belongs here. */
            int k;

            v = NULL;
            for (k = 0; k < g_ntoc; k++)
                if (g_toc[k].num == track && g_toc[k].length > 0) {
                    int sec = g_toc[k].length / 75;

                    sprintf(tmp, "%d:%02d", sec / 60, sec % 60);
                    v = tmp;
                    break;
                }
        }

        if (v == NULL || v[0] == '\0')
            continue;           /* skipped, not a gap */

        if (n > 0 && n < (int) sizeof out - 4) {
            strcpy(out + n, " - ");
            n += 3;
        }
        strncpy(out + n, v, sizeof out - n - 1);
        n += strlen(v);
        if (n > (int) sizeof out - 4)
            break;
    }

    gtk_label_set_text(GTK_LABEL(g_info),
                       out[0] ? out : STR_CDG_TEXT_NO_DISC_INFORMATION);
}

GtkWidget *
cdg_build(void (*report_fn)(const char *))
{
    GtkWidget   *vbox, *frame, *row, *align;
    GtkTooltips *tips;

    g_report = report_fn;
    cdg_reset(&g_screen);

    /*
     * THE SAVED OPTIONS, BEFORE ANY WIDGET READS THE STATICS - design/47
     * G5, 2026-10-01. The dialog stored its choices in these statics
     * and nothing read them back at the next start; the template had
     * the keys and nothing filled them. vlhe_cdg_prefs() validates, so
     * a hand-edited value cannot reach a widget as nonsense.
     */
    {
        struct vlhe_cdg_prefs pr;

        if (vlhe_cdg_prefs(&pr) == 0) {
            g_zoom           = pr.zoom;
            g_anchor         = pr.anchor;
            g_track_format   = pr.track_format;
            g_border_mask    = pr.border_mask;
            g_clear_on_eject = pr.clear_on_eject;
            g_compact_detached = pr.compact_detached;
            strncpy(g_info_fields, pr.info_fields, sizeof g_info_fields - 1);
            g_info_fields[sizeof g_info_fields - 1] = '\0';
        }
    }

    /*
     * TIGHT, AND MEASURED RATHER THAN CHOSEN. At 2x the picture is
     * 432px plus its frame in a 522px pane, so the border and the
     * inter-widget gaps are the whole budget for a transport row AND
     * an information line. At 8px border with 6px spacing the info
     * label fell off the bottom and the pane grew scrollbars - the
     * user saw it, "we are just a few pixels oversize".
     *
     *   border 4 top + 4 bottom            8
     *   picture 432 + frame shadow         436
     *   two gaps of 2                      4
     *   transport row                      ~30
     *   information line                   ~17
     *                                      ---
     *                                      495   in 522
     *
     * Raising the border or the spacing again means dropping to 1x
     * or losing a row; this is not slack to spend.
     */
    vbox = gtk_vbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 4);

    /*
     * THE PICTURE, CENTRED AND FIXED AT 2x.
     *
     * A GtkAlignment rather than packing it expanded: the frame is
     * 600x432 and the pane is 622x522, so it must not stretch to fill
     * - CD+G's 6x12 cells stay crisp only at integer scale, which is
     * design/32's reason for 2x in the first place.
     */
    /*
     * xalign 0.5 CENTRES, 0.0 puts it left. The user's option, and
     * the default is centred because that is what a player looks
     * like; left matters when the window is wider than the picture
     * and the eye wants the controls under its left edge.
     */
    {
        gfloat xa, ya;

        anchor_align(g_anchor, &xa, &ya);
        align = gtk_alignment_new(xa, ya, 0.0, 0.0);
    }
    g_picture = align;
    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);

    g_area = gtk_drawing_area_new();
    gtk_drawing_area_size(GTK_DRAWING_AREA(g_area),
                          VIEW_W(g_zoom), VIEW_H(g_zoom));
    gtk_signal_connect(GTK_OBJECT(g_area), "expose_event",
                       GTK_SIGNAL_FUNC(on_expose), NULL);
    /* THE BACKING STORE NEEDS A WINDOW, so it cannot be made here -
     * gdk_pixmap_new() wants a realised drawable. Hooking "realize"
     * is what makes the viewer draw at all; an exported function for
     * the shell to call was the first attempt and the shell never
     * called it, which showed up as a blank grey frame. */
    gtk_signal_connect(GTK_OBJECT(g_area), "realize",
                       GTK_SIGNAL_FUNC(on_realize), NULL);
    gtk_container_add(GTK_CONTAINER(frame), g_area);
    gtk_container_add(GTK_CONTAINER(align), frame);
    /*
     * THE PICTURE'S REGION EXPANDS; the controls below do not.
     *
     * TRUE, TRUE here is what pins the transport row and the
     * information line to the BOTTOM of the pane - they take their
     * natural height and this takes everything left over, so the
     * picture has a rectangle to anchor within and the controls stay
     * put. The user's framing: "the controls and info stay where it
     * is at the bottom and only the cd+g window moves".
     *
     * It matters more than the 800x600 case suggests. At 1024x768
     * fullscreen the pane is ~846x690, so even at 2x there is ~250px
     * of slack and the vertical third of the anchor does real work.
     */
    gtk_box_pack_start(GTK_BOX(vbox), align, TRUE, TRUE, 0);

    /*
     * THE TRANSPORT ROW, in the order a CD player has them:
     *
     *     |<   <<   [>]   [ ]   >>   >|
     *
     * with the two guards outermost, because they are the ones that
     * move by a whole track and a mis-click on them costs most.
     */
    tips = gtk_tooltips_new();
    row  = gtk_hbox_new(FALSE, 4);
    g_row1 = row;

    gtk_box_pack_start(GTK_BOX(row),
        icon_button(ICON_PREV, STR_CDG_BTN_PREV_TIP, GTK_SIGNAL_FUNC(on_prev), tips),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
        icon_button(ICON_RW, STR_CDG_BTN_RW_TIP, GTK_SIGNAL_FUNC(on_rw), tips),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
        icon_button(ICON_PLAY, STR_CDG_BTN_PLAY_TIP, GTK_SIGNAL_FUNC(on_play), tips),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
        icon_button(ICON_STOP, STR_CDG_BTN_STOP_TIP, GTK_SIGNAL_FUNC(on_stop), tips),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
        icon_button(ICON_FF, STR_CDG_BTN_FF_TIP, GTK_SIGNAL_FUNC(on_ff), tips),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row),
        icon_button(ICON_NEXT, STR_CDG_BTN_NEXT_TIP, GTK_SIGNAL_FUNC(on_next), tips),
        FALSE, FALSE, 0);

    /* OPTIONS, on the transport row because there is nowhere else -
     * a MOD_VIEW page has no tabs and no button row, and at 2x the
     * picture leaves 28px below. The row has ~200px spare. */
    {
        GtkWidget *opt = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_OPTIONS), STR_CDG_BTN_OPTIONS_TIP);

        gtk_signal_connect(GTK_OBJECT(opt), "clicked",
                           GTK_SIGNAL_FUNC(on_options), NULL);
        gtk_box_pack_start(GTK_BOX(row), opt, FALSE, FALSE, 12);

        g_hide_btn = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_HIDE), STR_CDG_BTN_HIDE_TIP);
        gtk_signal_connect(GTK_OBJECT(g_hide_btn), "clicked",
                           GTK_SIGNAL_FUNC(on_hide_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(row), g_hide_btn, FALSE, FALSE, 0);

        g_detach_btn = vlhe_tipped(gtk_button_new_with_label(STR_CDG_BTN_DETACH), STR_CDG_BTN_DETACH_TIP);
        gtk_signal_connect(GTK_OBJECT(g_detach_btn), "clicked",
                           GTK_SIGNAL_FUNC(on_detach_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(row), g_detach_btn, FALSE, FALSE, 0);

    }

    /*
     * THE TRACK DROPDOWN, to the RIGHT of the transport - the user's
     * layout, 2026-09-20: controls left, dropdown right.
     */
    /* AN OPTION MENU, LIKE SOUND SETTINGS' "Play through:" - see
     * on_track_picked() for how a row becomes a track. The items
     * and their handlers are built by tracks_load(). */
    /*
     * A PICKER, NOT A GtkOptionMenu - 2026-09-29, and the reason is
     * 99 tracks. GTK 1.2 menus do not scroll (gtkmenu.c has no
     * scroll code), so an option menu holding a full Red Book disc
     * asks for 2360px and everything past the screen edge is
     * unreachable - about 74 of 99 on the target. vlhe_picker.h has
     * the full account and why GtkCombo could not be bent into
     * doing it either.
     *
     * 240 IS THE POPUP CAP: about 12 rows at the target's font,
     * which fits under the control row without reaching the bottom
     * of a 600px screen.
     */
    g_tracks = vlhe_picker_new(NULL, 0, 240, on_track_picked, NULL);
    gtk_widget_set_usize(g_tracks, 260, -1);
    gtk_box_pack_end(GTK_BOX(row), g_tracks, FALSE, FALSE, 0);

    /* PACKED FULL WIDTH, not centred: the transport is at the left
     * and the dropdown at the right, so the row must span the pane. */
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    /* THE COMPACT SECOND ROW, empty and hidden until layout_rows()
     * moves Attach and the track list into it - the user's option,
     * 2026-10-01. Built here so it sits under the controls in the
     * same vbox that detaches. */
    g_row2 = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), g_row2, FALSE, FALSE, 0);

    /*
     * THE INFORMATION LINE, under everything. ONE LINE is what fits
     * at 2x - the picture takes 436 of the pane's 522px - and the
     * width carries about 86 characters. Which fields, and in what
     * order, is the Options dialog's two-list widget.
     */
    /*
     * THE TIME LEADS THE INFORMATION LINE - the user's layout,
     * 2026-09-28: *"can you move the --:-- at the start of the info
     * line insted of beside the controls and the drop down box"*.
     *
     * A SEPARATE LABEL IN AN HBOX, not a prefix inside `info_load()'.
     * That function rebuilds the whole line from the field list and
     * runs on a track change; the clock ticks once a second. Putting
     * the time inside it would mean either rebuilding the fields
     * every second or having two writers for one label.
     *
     * FIXED WIDTH so the text beside it does not shuffle as the
     * digits change - "10:00" is the widest a CD track reaches.
     */
    {
        GtkWidget *irow = gtk_hbox_new(FALSE, 8);

        g_time = gtk_label_new(STR_CDG_LABEL_TEXT);
        gtk_misc_set_alignment(GTK_MISC(g_time), 0.0, 0.5);
        gtk_widget_set_usize(g_time, 48, -1);
        if (tips != NULL)
            vlhe_tip(g_time, STR_CDG_TIP_ELAPSED_TIME_CURRENT_TRACK);
        gtk_box_pack_start(GTK_BOX(irow), g_time, FALSE, FALSE, 0);
        gtk_widget_show(g_time);

        g_info = gtk_label_new(STR_CDG_LABEL_NO_DISC_INFORMATION);
        gtk_misc_set_alignment(GTK_MISC(g_info), 0.0, 0.5);
        gtk_label_set_justify(GTK_LABEL(g_info), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(irow), g_info, TRUE, TRUE, 0);
        gtk_widget_show(g_info);

        gtk_box_pack_start(GTK_BOX(vbox), irow, FALSE, FALSE, 0);
        gtk_widget_show(irow);
    }

    /*
     * THE SLOT IS WHAT THE PAGE HOLDS, and the vbox lives inside it.
     *
     * A GtkHandleBox was here first and WORKED - the user confirmed
     * the drag - but it cannot be driven from code: `child_detached'
     * is a read-only status bit and the header exposes no detach
     * call, so a button was impossible. Reparenting by hand needs a
     * fixed place to put the child back, which is this.
     */
    g_vbox = vbox;
    /*
     * THE SLOT JUST HOLDS THE VBOX, FULL HEIGHT.
     *
     * An earlier version centred the whole group here, which moved
     * the controls as well as the picture - not what was wanted. The
     * vbox now gives its expanding space to the picture's alignment
     * and the controls sit at the bottom by construction.
     */
    g_slot = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_slot), vbox, TRUE, TRUE, 0);

    /*
     * NO DEMO CONTENT ANY MORE - removed 2026-09-26 when the page
     * gained a real disc.
     *
     * A fixture block sat here showing "Joyful Nights" by the
     * "United Cat Orchestra", the libcdio corpus's own strings, and
     * its comment said "UNTIL A DISC IS WIRED IN". That was honest
     * scaffolding while the transport reached nothing; the moment
     * it did, the page would have shown those names over a
     * correctly decoding picture of somebody's actual disc.
     *
     * `image_follow()' now fills the list from `img->text', which
     * the CUE parser has been populating all along, and leaves it
     * empty - `<Unknown>', KsCD's convention - when a disc carries
     * no CD-TEXT.
     */

    gtk_widget_show_all(g_slot);

    /*
     * THE HIDE BUTTON IS HIDDEN BY cdg_sync_visibility(), NOT HERE.
     *
     * The shell calls gtk_widget_show_all() on the page AFTER this
     * function returns (vlhe_cc.c:2346), and show_all is RECURSIVE -
     * it reveals every descendant, undoing any hide a module made
     * during its own build. vlhe_cc.c:2350 has the account: the
     * Volume module lost the same argument and the answer was a
     * sync_visibility() hook called after show_all. This is the
     * fourth instance of that trap in this project and the first
     * where the module knew about it in advance.
     */
    /*
     * THE POLL STARTS WITH THE PAGE AND RUNS FOR THE PROGRAM'S
     * LIFE, like the Volume module's (`vlhe_mod_volume.c:1207').
     * It costs one ioctl every 250 ms while a disc is PLAYING and
     * nothing at all otherwise - `on_poll' returns early when the
     * page is inactive or no drive has been chosen.
     */
    gtk_timeout_add(CDG_POLL_MS, on_poll, NULL);

    gtk_signal_connect(GTK_OBJECT(g_slot), "size_allocate",
                       GTK_SIGNAL_FUNC(on_slot_allocate), NULL);   /* D60 */
    return g_slot;
}

/*
 * RE-APPLY WHAT show_all TRAMPLED. Called by the shell after its own
 * gtk_widget_show_all(), alongside volume_sync_visibility() and the
 * others.
 */
void
cdg_sync_visibility(void)
{
    if (g_hide_btn == NULL)
        return;

    /* Hide only makes sense on a detached window - on the page it
     * would leave a MOD_VIEW with nothing to view. */
    if (g_float != NULL)
        gtk_widget_show(g_hide_btn);
    else
        gtk_widget_hide(g_hide_btn);
}


