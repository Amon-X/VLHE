/*
 * vlhe_backend.h - what the control centre is allowed to know about the
 * system. THE GUI TALKS TO NOTHING ELSE.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * WHY THIS LAYER EXISTS. The window has to be iterated on - four
 * modules, eight tabs, two slider orientations - and every one of those
 * iterations would otherwise need a loaded vsound, a running vdiscd and
 * a machine that has them. That is a boot-and-carry cycle per layout
 * tweak, on hardware whose whole appeal is that it is slow.
 *
 * So: two implementations behind one header. The fake one returns
 * scripted state and runs anywhere, including on this workstation under
 * the target's loader (`make run-gui-cc`). The real one reads /proc,
 * the device nodes and the daemons, and is the only one that ships
 * enabled. The GUI cannot tell them apart, which is the point - a
 * layout proved against the fake is proved, because the widget code
 * never branched on which backend it had.
 *
 * WHAT GOES IN HERE AND WHAT DOES NOT. This is state, not policy. The
 * backend says "four channels, the third is muted, its label is
 * 'MIDI Synth'". It does not say how wide the slider is or which tab
 * the channel appears on. If a function here would have to know what a
 * GtkNotebook is, it belongs in the module instead.
 *
 * EVERY CALL CAN FAIL, and the GUI must stay usable when it does. A
 * machine with no vsound loaded is a machine whose control centre still
 * opens - showing nothing rather than refusing to start. So the getters
 * return a count that may be zero, never an error the caller has to
 * handle before it can draw a window.
 *
 * C89, GCC 2.95.2. No GTK types appear in this file.
 */

#ifndef VLHE_BACKEND_H
#define VLHE_BACKEND_H

/* ROOM FOR EVERY SLOT the kernel can list. vsound.h's VSOUND_LIST_CHAN
 * is the real bound; this header does not include it because the fake
 * backend must build without the driver's headers. Kept deliberately
 * larger than any configuration we ship, and vlhe_channels() reports
 * how many of these are real. */
#define VLHE_MAX_CHAN   8
/* VDISC_MAX_DEVS - the module's own ceiling. The GUI draws
 * vlhe_drive_count() rows, not this many. */
#define VLHE_MAX_DRIVE  8
#define VLHE_NAME_MAX   32
/* THE BOOST CEILING, in per cent - vsound.h's VSOUND_VOL_BOOST, which
 * the build passes to every compile as -DVSOUND_VOL_BOOST (defs.mk,
 * `make VOL_BOOST='). Restated rather than included for the reason
 * above; 100 is unity, and the GUI's sliders reach this with Boost. */
#ifdef VSOUND_VOL_BOOST
#define VLHE_VOL_BOOST  VSOUND_VOL_BOOST
#else
#define VLHE_VOL_BOOST  200
#endif
#define VLHE_PATH_MAX   256

/* ------------------------------------------------------------------ */
/* Channels - the Volume module                                       */
/* ------------------------------------------------------------------ */

struct vlhe_channel {
    int  index;                     /* the slot number, for the driver  */
    int  pid;                       /* whose stream - 0 when free       */
    char name[VLHE_NAME_MAX];       /* the client's command name        */
    int  volume;                    /* 0-100, what the slider shows     */
    int  muted;                     /* 0 or 1                           */
    int  active;                    /* is something actually playing on
                                     * it - drawn as state, never used
                                     * to hide the row */
    int  midi;                      /* the reserved MIDI slot           */
};

/* THE PID IS NOT DECORATION - IT IS WHAT MAKES A WRITE SAFE.
 *
 * vsound reuses channel indices: a client exits, another opens, and the
 * slot number is the same. A GUI holding "index 1 = lxdoom" from a poll
 * 250ms ago would otherwise set QUAKE's volume after doom exited. The
 * kernel checks the pid under one cli() (vsound.h, VSOUND_IOC_VOL), so
 * passing it back closes the window rather than narrowing it.
 *
 * So vlhe_set_volume() takes the pid the caller last saw, and -ESRCH
 * from the driver means "that slot changed hands" - a refresh, not an
 * error to show. */

/* GENERATION - has anything moved?
 *
 * vsound bumps a counter on every volume change, open AND close, so a
 * poll is one ioctl and one integer compare: no redraw and no X traffic
 * unless something actually changed. That is what makes polling at
 * 250ms cheap enough not to argue about.
 *
 * Returns the counter, or 0 when the module is absent. */
unsigned long vlhe_generation(void);

/* Fills up to max entries, returns how many were written. ZERO IS A
 * VALID ANSWER: no driver loaded, or nothing has opened it yet. */
int vlhe_channels(struct vlhe_channel *out, int max);

/* Returns 0 on success, -1 if the change could not be made. The GUI
 * shows the failure in the status bar; it does not roll the slider
 * back, because the next refresh will do that from real state. */
int vlhe_set_volume(int index, int pid, int volume);
int vlhe_set_mute(int index, int pid, int muted);

/* ------------------------------------------------------------------ */
/* Drives - the CD Settings module                                    */
/* ------------------------------------------------------------------ */

struct vlhe_drive {
    int  index;                     /* minor number                     */
    char node[VLHE_PATH_MAX];       /* "/dev/vdisc0"                    */
    char image[VLHE_PATH_MAX];      /* attached image, "" if empty      */
    int  attached;                  /* 0 or 1                           */
    int  tracks;                    /* from the TOC, 0 when empty       */
    int  audio_tracks;              /* how many of those are audio      */
    int  sessions;                  /* 1, or more on a multi-session    */
    int  mounted;                   /* something has it open            */
    int  autoload;                  /* re-attach this image at startup  */

    /* WHAT KIND OF DISC, in the words a user would recognise - "Mode 1",
     * "Mode 2 / XA", "Mixed Mode", "Audio". Built by the backend from
     * the tracks' sector_form and is_data, because the interesting
     * answer is about the DISC and the caller should not have to derive
     * it from a track list it does not have. Empty when no disc. */
    char kind[VLHE_NAME_MAX];

    /* 1 IF /dev/cdrom REACHES THIS DRIVE - through vdiscd's inner link,
     * 2026-10-03. Exactly one drive has it when the daemon answers.
     * LAST, so the fake's positional tables need not change. */
    int  cdrom;
};

/*
 * POINT /dev/cdrom AT ANOTHER DRIVE, LIVE - the CD page's "/dev/cdrom"
 * radio (2026-10-03). Asks vdiscd to move the inner link
 * (vdiscd_ctl.h); a program with the old drive open keeps it until it
 * reopens. Recorded by the daemon for the next load. Only meaningful
 * with [CD Settings] LinkCdrom on - otherwise /dev/cdrom is not ours
 * and the inner link reaches nothing. 0, or -1 if the daemon refused
 * or did not answer.
 */
int vlhe_set_cdrom_drive(int index);

/* HOW MANY DRIVES THIS MACHINE ADVERTISES - the module's `discs='
 * parameter, VDISC_DEF_DEVS (1) unless asked otherwise, ceiling
 * VDISC_MAX_DEVS (8).
 *
 * THE GUI DRAWS THIS MANY ROWS. The mockup shows one drive and one
 * image path; the module has advertised a configurable count since it
 * was written, and discs attach and detach into those slots at runtime
 * with no reload - only raising the COUNT needs one, because the
 * CD-ROM layer registers each drive by name at init.
 *
 * Returns 0 when vdisc is not loaded. */
int vlhe_drive_count(void);
/* Drop its cached answer - after a module step, since the count is
 * fixed at insmod and the cache otherwise outlives a reload (B5). */
void vlhe_drive_count_forget(void);

int vlhe_drives(struct vlhe_drive *out, int max);
/* Drop its kept answer (DRIVES_KEEP_MS, 2026-10-03) so the next call
 * asks vdiscd. The backend calls it on every change it makes; a page
 * needs it only for a change made some other way. */
void vlhe_drives_forget(void);
/* FOR THE HOST TESTS: one drive's token of the daemon's status reply,
 * after its `N=' - `T/A:path' (C5) or `path'. Returns 1 when the
 * track counts were in it. */
int vlhe_status_token_parse(const char *p, struct vlhe_drive *d);
/*
 * PUT AN IMAGE IN A DRIVE, through vdiscd. 0 on success; otherwise
 * WHY, so the CD page can say the right thing (2026-10-05 - every
 * failure was -1, and a refusal from a running daemon was reported as
 * "check that vdiscd is running"):
 */
#define VLHE_ATTACH_NO_ANSWER  (-1)   /* vdiscd did not answer          */
#define VLHE_ATTACH_DENIED     (-2)   /* it cannot read the file        */
#define VLHE_ATTACH_MISSING    (-3)   /* it finds no such file          */
#define VLHE_ATTACH_REFUSED    (-4)   /* any other refusal: not an image
                                       * it understands, drive in use  */
int vlhe_attach(int index, const char *path);
/* THE REPLY'S MEANING - vlhe_attach()'s second half, for the tests */
int vlhe_attach_reply(const char *reply);
int vlhe_detach(int index);

/*
 * THE CD TRANSPORT - design/34 6e2.
 *
 * Play, stop, pause and resume go through `vdiscd', because nothing
 * else in userspace plays CD audio: its CDDA child does, and until
 * 2026-09-26 it was only ever reached through the kernel's
 * CDROMPLAYMSF. `track' is 1-based as printed on the disc.
 *
 * PLAY RUNS TO THE LEAD-OUT, not to the end of the track, which is
 * what a real player does. Send stop for one track only.
 *
 * 0 on success, -1 otherwise.
 */
/*
 * `secs' IS AN OFFSET INTO THE TRACK, and it is how seeking is done
 * - KsCD has no seek command either, it re-issues PLAY from a
 * different point (`kscd.cpp:786'). 0 starts at the beginning.
 *
 * Forward past the track's end is REFUSED by the daemon rather than
 * clamped, as KsCD refuses it (`:787'): running into the next track
 * is not what a seek button means.
 */
int vlhe_cd_play(int index, int track, int secs);
int vlhe_cd_stop(int index);
int vlhe_cd_pause(int index);
int vlhe_cd_resume(int index);

/*
 * WHERE PLAYBACK HAS REACHED, as an absolute LBA, or -1.
 *
 * FROM THE KERNEL RATHER THAN THE DAEMON, and that is not an
 * optimisation: the CDDA child pushes its position straight to the
 * module so that a wedged child cannot block the block server
 * (`vdiscd_play.c:45'), so the parent never sees it and cannot be
 * asked. This reads CDROMSUBCHNL on /dev/vdiscN, which is where the
 * module caches it.
 *
 * IT IS WHAT THE CD+G VIEWER POLLS. The viewer decodes the
 * subchannel from the image itself and needs only a position to
 * decode TO.
 *
 * `track' and `playing' are filled when non-NULL.
 */
int vlhe_cd_position(int index, int *track, int *playing);

/*
 * ONE TRACK OF THE TOC. Sector addresses, file-relative, as the
 * image holds them - the same numbers `CDROMSUBCHNL' returns, so
 * the two can be subtracted without conversion.
 */
struct vlhe_cd_track {
    int num;                /* as printed on the disc, 1-based      */
    int start;              /* first sector                         */
    int length;             /* sectors                              */
    int is_data;
};

/*
 * READ A DRIVE'S TOC - and the CLIENT KEEPS IT, which is the point.
 *
 * KsCD's arrangement (`cdrom.c:431'): a track-relative position is
 * `(frame - trk[n].start) / 75' computed locally, from a TOC read
 * once at attach, with CDROMSUBCHNL for the frame. No daemon is
 * asked and no seek command exists anywhere in it.
 *
 * SO THERE IS NO `vlhe_cd_seek()'. One was written and removed: it
 * needed a track's start LBA, which nothing here could supply, and
 * the fix is this function rather than moving the arithmetic to the
 * party that has the TOC open. Seek is
 *
 *     vlhe_cd_play(drive, track, secs_in_track + delta)
 *
 * with the seconds worked out from a TOC the caller already holds.
 *
 * Returns tracks filled, or -1. Read it ONCE per attach: nothing
 * about a disc's layout changes while it is in the drive.
 */
int vlhe_cd_toc(int index, struct vlhe_cd_track *out, int max);

/* NO DRIVE LOCK - vlhe_set_locked() and vlhe_locked() were removed
 * 2026-10-05 with the CD page's Lock box (design/54 D64). 2.2's lock is
 * one flag for every CD-ROM (cdrom.c:260), so a per-drive box locked
 * them all and outlived the Unload; and an eject of a drive in use is
 * already refused - by the CD-ROM layer for a program's eject
 * (`use_count != 1', cdrom.c:1466) and by vdisc_do_detach() for ours. */


/* RE-ATTACH THIS IMAGE WHEN THE DAEMON NEXT STARTS.
 *
 * NOT "AUTO-MOUNT", which is what the mockup called it. Mounting a
 * filesystem is /etc/fstab's job and nothing here should compete with
 * it; what this controls is whether the DISC IS IN THE DRIVE when
 * vdiscd comes up. A user who wants it mounted as well writes an
 * fstab line, exactly as they would for a real drive.
 *
 * PER DRIVE, because the slots are independent: one machine may want
 * its game disc back every boot and a scratch slot left empty.
 *
 * RECORDED BY vdiscd IN THE DRIVE STATE (2026-10-03, design/36 row
 * 64), not the config file: the box asks the daemon over the channel,
 * as an attach does, and the daemon writes /var/lib/vlhe/state/drives - or
 * `drives' in a portable folder. The click acts at once on the stored
 * flag; the EFFECT is at the next load (`vdiscd -r', when [CD
 * Settings] DrivesAutoLoad = 1), which the status line says. 0, or -1
 * when the daemon is not there to record it. */
int vlhe_set_autoload(int index, int on);

/* ------------------------------------------------------------------ */
/* Recently attached images                                           */
/* ------------------------------------------------------------------ */

/* TEN, LEAST-RECENTLY-USED, so the file picker is only needed for a
 * disc the user has not opened before.
 *
 * LRU AND NOT FIFO, and the difference is the point: re-attaching an
 * image already in the list MOVES IT TO THE FRONT rather than adding a
 * second entry. Under FIFO a disc used every day would still be
 * evicted by the eleventh new one because it went in first.
 *
 * ONLY A SUCCESSFUL ATTACH ENTERS THE LIST. A list of paths that did
 * not work is worse than no list, and an image that has been moved or
 * deleted would otherwise sit in it permanently.
 *
 * PERSISTED IN THE PER-USER CONFIG FILE, not here - it is a record of
 * what THIS user has opened, and a list that does not survive a
 * restart is not worth keeping. design/09 settles the format. */
#define VLHE_MAX_RECENT 10

/* Fills up to max paths, most recent first. Returns how many. */
int vlhe_recent(char out[][VLHE_PATH_MAX], int max);

/* Drop one path from the list.
 *
 * FOR THE CASE WHERE AN IMAGE HAS MOVED. The GUI asks the user first
 * rather than dropping it silently, because a path that is absent
 * today may be back tomorrow - an image on removable media, or a
 * volume that is not mounted yet. Removing it is the user's decision,
 * not a side effect of one failed attach. */
int vlhe_recent_remove(const char *path);

/* ------------------------------------------------------------------ */
/* vdisc's module parameters - the CD Options tab                     */
/* ------------------------------------------------------------------ */

/* THESE ARE NOT LIVE SETTINGS. Every one is read at init_module and
 * takes effect only when the module is next loaded - `vdisc_ndevs'
 * because the CD-ROM layer registers each drive BY NAME at init, and
 * the others for the same reason. So the tab is a FORM: it writes the
 * config file, OK/Apply commit, and the change applies on the next
 * load. That is the opposite of the Drive tab, where everything acts
 * at once.
 *
 * AND THEY NEED ROOT, unlike everything else in this program.
 * design/09's privilege model puts settings on the unprivileged side
 * and ADMINISTRATION - insmod, rmmod, daemons - on the other. Changing
 * a module parameter is administration, so the tab says so rather than
 * letting Apply fail with a permission error the user cannot act on.
 *
 * `applied' is what the RUNNING module has; the rest is what the
 * config file says the next load should use. When they differ the GUI
 * says a reload is needed. */
struct vlhe_modopts {
    int  ndevs;                 /* drives to advertise, 1..8           */
    int  ndevs_applied;         /* what the RUNNING module has; 0 if
                                 * not loaded                           */
    int  major;                 /* block major, 63 by default          */
    int  major_applied;         /* the major vdisc holds in
                                 * /proc/devices; 0 if not loaded.
                                 * design/47 B2: was never assigned     */
    int  mimic;                 /* is the impersonation list in use -
                                 * DERIVED from `major' being one of
                                 * the VLHE_MAJ_MIMIC list, so it
                                 * needs no key of its own (B2/C2)     */
    int  packet;                /* CDC_GENERIC_PACKET on or off        */
    int  packet_applied;        /* -1 ALWAYS: the running module does
                                 * not report its vdisc_packet, so a
                                 * reader must not compare it (B2)     */
    /*
     * POINT /dev/cdrom AT THE VIRTUAL DRIVE, and put it back on
     * unload. OFF BY DEFAULT - it changes a path outside anything we
     * own, and a user who did not ask for that must not get it.
     *
     * COREL DOES SHIP THE SYMLINK, measured rather than inferred -
     * `tests/vm/fresh.img', the untouched Open Circulation install,
     * has `/dev/cdrom' as a symlink to `/dev/hdb'. An earlier version
     * of this comment said it did not, reasoning from an `/etc/fstab'
     * that names `/dev/hdc' directly; that file says what Corel
     * MOUNTS and cannot testify about what exists in /dev.
     *
     * SO THIS OPTION REDIRECTS A LINK THAT ALREADY WORKS, pointing at
     * the user's REAL DRIVE, and KsCD hardcodes that path (CLAUDE.md
     * section 5). That is a bigger intervention than supplying
     * something absent - which is why the default is off and why the
     * restore is the feature rather than a nicety.
     *
     * THE RESTORE IS THE FEATURE. The user, 2026-09-23: "I have run
     * into times where I forgot it was set and had to swap it back to
     * my real drive." A symlink is invisible state that outlives the
     * session that made it, which is why what it pointed at is saved
     * to a FILE and why the Status page shows the current state.
     */
    int  link_cdrom;
    /*
     * [CD Settings] DrivesAutoLoad - REATTACH THE DRIVES MARKED "Load
     * at startup" WHEN THE DISC SERVER STARTS (vdiscd -r). On by
     * default. The master switch for every drive's own box; the CD
     * page's Options tab has it since 2026-10-06 (the user, "3 fix":
     * the drive box's tip sent users to setup-vlhe for it).
     */
    int  drives_autoload;
    int  loaded;                /* is vdisc there to compare against   */
};

/*
 * WHAT `/dev/cdrom' IS RIGHT NOW - for the Status page, which shows
 * it because the user's complaint was forgetting it was set.
 *
 * `target' is what the symlink points at, `saved' what we recorded
 * before changing it (empty when we did not change it), and `kind'
 * says why we may have left it alone.
 */
#define VLHE_CDROM_ABSENT   0   /* nothing at /dev/cdrom              */
#define VLHE_CDROM_SYMLINK  1   /* a symlink - ours to manage         */
#define VLHE_CDROM_NODE     2   /* a REAL device node - left alone    */
#define VLHE_CDROM_OTHER    3   /* a directory, a file, something odd */

struct vlhe_cdrom_link {
    int  kind;
    char target[VLHE_PATH_MAX];  /* what it points at, if a symlink   */
    char saved[VLHE_PATH_MAX];   /* what we will restore it to        */
    int  is_ours;                /* does it point at a vdisc node, or
                                  * at our inner link (2026-10-03)    */
    char via[VLHE_PATH_MAX];     /* the drive behind the inner link,
                                  * "" when it points elsewhere       */
    char free_name[64];          /* an unused /dev/cdromN, when the   */
                                 /* real-node case forces a refusal   */
};

int vlhe_cdrom_link(struct vlhe_cdrom_link *out);

/*
 * WHERE THE SAVED TARGET IS KEPT. Installed, `/var/lib/vlhe/cdrom.was'
 * beside `drives'; a portable copy keeps it in its own folder, because
 * one that promises to touch nothing of the system must not write to
 * /var. `VLHE_CDROM_SAVED' overrides, which is what makes the apply
 * path testable on a workstation.
 */
/* WHERE WE REMEMBER WHAT /dev/dsp WAS - design/38 section 9. Same
 * shape as the cdrom one: a file, because the process that made the
 * change may not be the one that undoes it. */
const char *vlhe_dsp_saved_path(void);

const char *vlhe_cdrom_saved_path(void);

#define VLHE_MIN_NDEVS  1
#define VLHE_MAX_NDEVS  8       /* VDISC_MAX_DEVS */
#define VLHE_MAX_MAJORS 16

/* THE MAJORS THE GUI IS WILLING TO OFFER, already filtered.
 *
 * NOT A NUMERIC FIELD - design/27's decision, and the reason is a
 * failure whose symptom appears far from its cause:
 *
 *   real driver first, then vdisc   our insmod fails. Fine: clean,
 *                                   immediate, obviously ours.
 *   VDISC FIRST, THEN REAL DRIVER   THE USER'S HARDWARE STOPS
 *                                   WORKING, and they debug the wrong
 *                                   module.
 *
 * "A free-text box invites someone to type 3 and collide with their
 * boot disk." So the backend hands back a SHORT LIST, each entry
 * naming the hardware it impersonates, with anything already
 * registered EXCLUDED - read from /proc/devices at the moment of
 * asking, not cached.
 *
 * A FILTER IS NOT A GUARANTEE: a major free when the menu is built can
 * be claimed by a driver loaded afterwards. design/27 pairs this with
 * a load-time message from the module naming the family it collided
 * with. That message is not built.
 *
 * ARBITRARY MAJORS ARE A COMPILE-TIME OPTION, deliberately not a
 * gated runtime field - "the people most likely to open an advanced
 * panel are the least likely to read the warning". The module
 * parameter stays open to anyone typing insmod by hand; this governs
 * only what the GUI will offer. */
struct vlhe_major {
    int  num;
    char what[64];              /* "Matsushita / Panasonic CR-5xx"     */
    int  in_use;                /* registered already - offered but
                                 * marked, never silently dropped     */
};

/* TWO LISTS, NOT ONE.
 *
 * VLHE_MAJ_LOCAL  60-63, the kernel's LOCAL/EXPERIMENTAL range
 * VLHE_MAJ_MIMIC  the period CD-ROM drivers, for cdparanoia only
 *
 * devices.txt:1005 reserves 60-63 "for devices not assigned official
 * numbers... in order to avoid conflicting with future assignments",
 * which is exactly us - and it is what the reference implementation
 * uses (`virtualcd_0.3/virtualcd.c:26`, MAJOR_NR 63, commented
 * "LOCAL/EXPERIMENTAL US").
 *
 * 44 WAS THE OLD DEFAULT AND WAS NEVER OURS. design/05 recorded it as
 * "block major 44 (free)", which was true of that machine and not of
 * the assignment table: 44 belongs to Flash Translation Layer
 * filesystems. Harmless on Corel 1.2, where nobody has FTL hardware,
 * but borrowed rather than allocated.
 *
 * THE SECOND LIST IS AN OPT-IN, behind a checkbox, because taking a
 * real driver's major is how a user's own hardware stops working -
 * and the failure appears far from its cause. design/27 has the
 * ordering table. */
#define VLHE_MAJ_LOCAL  0
#define VLHE_MAJ_MIMIC  1

int vlhe_majors(int which, struct vlhe_major *out, int max);

int vlhe_modopts(struct vlhe_modopts *out);
int vlhe_set_modopts(const struct vlhe_modopts *in);

/* Can this user change them? design/09: administration needs root, and
 * the desktop's mechanism is kappsu - a SECOND desktop entry running
 * the whole program as root, not a program asking for a password when
 * it needs one. So the GUI shows the fields and says they are not
 * editable rather than pretending otherwise. */
int vlhe_can_administer(void);

/* ------------------------------------------------------------------ */
/* Committing - what makes a setting outlive the process              */
/* ------------------------------------------------------------------ */

/*
 * EVERY vlhe_set_* ABOVE WRITES MEMORY ONLY. Nothing reaches a file
 * until vlhe_commit() is called, which is what Apply and OK do.
 *
 * WHY THE SPLIT: a page holds several controls and a user edits them
 * in any order, so writing on each change would rewrite the file a
 * dozen times for one visit - and would leave a half-edited page
 * persisted if they then pressed Cancel. Apply is the commit point
 * design/32 section 8 already established for forms.
 *
 * AND IT WAS NOT TRUE UNTIL 2026-09-18. 21cd6ad had every setter end
 * with its own vlhe_conf_template_write(), so this paragraph
 * described an intention rather than the code. The consequence was a
 * DESTROYED BACKUP: the setter wrote the file, vlhe_commit() wrote it
 * again, and the second write copied the FIRST write aside - so
 * `.old' held the new value and the previous version was gone. Caught
 * by test_vlhe_commit.c the first time it ran, which is what that
 * test exists for.
 *
 * ONE FUNCTION STILL WRITES IMMEDIATELY AND SHOULD:
 * vlhe_recent_remove() - an action in a dialog that has already
 * closed, with no later commit point to carry it; annotated where it
 * writes. vlhe_set_prefs() was the second and is memory-only since
 * `0a3eb45' (the font applies live; File > Save keeps it) - corrected
 * 2026-10-03, design/54 T17.
 */

/* Is anything unsaved? 0 or 1.
 *
 * THIS IS NOT AN OPTIMISATION - IT PROTECTS THE BACKUP. design/33
 * keeps ONE `.old' per file, so an Apply with nothing changed would
 * copy the live file over its own backup and DESTROY the previous
 * version. Two idle clicks would lose it. So a clean commit writes
 * nothing at all.
 *
 * It also means a config a user edited by hand while this window sat
 * open survives, instead of being overwritten from a struct read at
 * startup.
 */
int vlhe_dirty(void);

/*
 * Write what has changed. Returns 0, or -1 with `err' filled in.
 *
 * ONLY THE FILES THAT CHANGED ARE WRITTEN. design/33's three files
 * hold disjoint keys, so a Sound Settings edit touches the system
 * file and leaves the user file alone - fewer chances to clobber
 * something edited by hand elsewhere.
 *
 * A CLEAN COMMIT IS A SUCCESSFUL NO-OP: returns 0, writes nothing,
 * and leaves `.old' as it was.
 */
struct vlhe_commit_err {
    char path[VLHE_PATH_MAX];       /* which file failed              */
    char why[160];                  /* strerror, or our own words     */
    /*
     * THE USER FILE COULD NOT BE WRITTEN WHERE IT BELONGS, AND HERE IS
     * THE NEXT PLACE TO OFFER - 2026-09-24, design/36 row 60.
     *
     * `user_tier' is the tier that failed (VLHE_USER_TIER_*, from
     * vlhe_conf.h) or -1 when the failure was not the user file.
     * `alt' is the next tier's path, empty when there is none. The
     * GUI ASKS before moving to $HOME and only NOTIFIES before /tmp;
     * the CLI names both and stops, because it cannot ask.
     */
    int  user_tier;
    char alt[VLHE_PATH_MAX];
    /*
     * THE MACHINE SETTINGS WENT TO THE DRAFT, NOT THE SYSTEM FILE -
     * design/51, 2026-10-01. Set, with the path, when a Save by a
     * user who cannot write the system file put that half in the
     * draft at its default path instead; the commit succeeded and
     * the dirty bit is clear. Empty otherwise.
     */
    char drafted[VLHE_PATH_MAX];
    /*
     * THE SYSTEM FILE WAS NOT MADE BECAUSE THIS IS NOT ROOT - the
     * expected state for an ordinary user, which the GUI's Save must
     * not show as a failure. A FLAG, NOT THE WORDING: save_config()
     * used to test strncmp(why, "needs root", 10) against text made
     * here, the design/47 section 7 fault 3 shape, which the strings
     * pass would have broken. 2026-10-02.
     */
    int  needs_root;
};

/*
 * `create' SAYS WHETHER A MISSING FILE MAY BE MADE.
 *
 * 0 - Apply. Settings are taken, and written ONLY to files that
 *     already exist. On a machine with no config this writes nothing
 *     and still succeeds: the settings are in memory and they are
 *     real, which is the whole no-config session model
 *     (design/33 section 1e).
 *
 * 1 - File / Save Configuration. Creates what is missing.
 *
 * THE SPLIT EXISTS BECAUSE APPLY CREATED A CONFIG THE USER HAD JUST
 * DECLINED, on 86Box 2026-09-19, and reported "Settings saved" as
 * though it had merely updated one. Pressing Apply is consent to
 * apply; it is not consent to start writing /etc.
 */
int vlhe_commit(struct vlhe_commit_err *err, int create);

/*
 * THROW AWAY EVERYTHING UNSAVED - Cancel's half, by re-reading the
 * files. Always succeeds; a missing file reads as an empty one.
 *
 * IT CAN ONLY UNDO WHAT WAS NEVER WRITTEN, which is the user's point
 * and worth stating where someone will read it: once Apply has run,
 * the value is on disk and Cancel cannot reach it. So Cancel after an
 * Apply discards only what changed SINCE that Apply.
 *
 * The caller must then call each module's reload() to put the widgets
 * back - this restores the backend, not the screen.
 */
void vlhe_discard(void);

/*
 * Is there any config file at all? 0 or 1, for the first-run prompt.
 *
 * NOTHING IS GATED ON ONE - design/33 section 1b, verified: a missing
 * file reads as an empty one, every key has a default, and the
 * daemons read no config whatsoever. So this asks "will settings
 * persist", never "can the program run".
 */
int vlhe_conf_exists(void);
/* One of them: VLHE_TPL_SYSTEM or VLHE_TPL_USER (vlhe_conf_template.h). */
int vlhe_conf_file_exists(int which);

/* May root act on the system config? 1 if so; 0 with the reason in
 * `why' if a user other than root could have written it - design/49
 * T0. vlhe_plan_run() refuses on 0. */
int vlhe_conf_system_trusted(char *why, int max);

/* Replace the "can this session be root for a write?" probe. FOR THE
 * HOST TESTS ONLY, which run as a user and must still reach the
 * system half; NULL restores the real one. Nothing shipped calls it. */
void vlhe_backend_set_root_probe(int (*probe)(void));

/* Treat the seed as root's for the system file. FOR THE HOST TESTS
 * ONLY, whose seed is necessarily the tester's; nothing shipped calls
 * it. */
void vlhe_backend_set_seed_trusted(int on);
/* And the system file itself - R1. FOR THE HOST TESTS ONLY: every file
 * a host test writes is the tester's and would otherwise never load. */
void vlhe_backend_set_config_trusted(int on);

/*
 * Create the config files with defaults, for the first-run prompt's
 * [Create]. Returns the number written, or -1 with `err' filled in.
 *
 * WHERE THE DEFAULTS COME FROM IS design/33 SECTION 1b: the SEED if
 * setup wrote one - what this machine was INSTALLED as - and the
 * compiled-in template otherwise. Read, never merged: one source or
 * the other, so there is no precedence rule here.
 *
 * WRITES WHAT IT CAN AND REPORTS WHAT IT COULD NOT. A user who is not
 * root cannot create /etc/vlhe.conf; that is not a failure of the
 * operation, it is a fact to show them, so the user file is still
 * created and `err' names the one that was skipped.
 */
int vlhe_conf_create(struct vlhe_commit_err *err);

/*
 * A DRAFT, AND LOADING ONE - design/49 T0.
 *
 * vlhe_conf_save_draft() writes the machine settings in memory to a
 * file of the caller's, as whoever is running - never the live config
 * or the seed. Returns 0, or -1 with `err'.
 *
 * LOADING ONE IS A REVIEW, THEN A DECISION. vlhe_conf_draft_review()
 * reads the file (a regular file only), refuses if anything is
 * already unsaved, and sets each key through its page's setter - the
 * one definition of valid - on a copy: the live settings do not move.
 * It fills `r' with what would change (old -> new), what was refused,
 * and which keys were skipped because no page takes them (paths and
 * CardModule among them); the user's own half is only counted.
 * vlhe_conf_draft_accept() installs the result AS UNSAVED EDITS - Save
 * still writes; vlhe_conf_draft_cancel() drops it. Returns 0 / -1.
 */
int vlhe_conf_save_draft(const char *path, struct vlhe_commit_err *err);

/*
 * THE DRAFT AT ITS DEFAULT PATH IS A NON-ROOT USER'S MACHINE
 * CONFIGURATION - design/51, the user's decisions of 2026-10-01.
 *
 * vlhe_draft_default_path(): beside the user file, or in $HOME -
 * "vlhe-draft.conf". Where Save Draft opens, where a non-root Save
 * puts the machine half, and what the next start reads back.
 *
 * vlhe_conf_draft_readback(): call once at start, after the privilege
 * hooks are set. Root: nothing. Otherwise, a draft at the default path
 * NEWER than the system file (or with none) is reviewed as an import
 * would be and installed NOT DIRTY; an older one is superseded and
 * left alone. Returns 1 taken, -1 found and not taken, 0 none;
 * `note' is a sentence either way. vlhe_discard() re-applies a draft
 * in use. vlhe_conf_draft_in_use() says which, for a standing notice.
 */
const char *vlhe_draft_default_path(void);
int  vlhe_conf_draft_readback(char *note, int max);
int  vlhe_conf_draft_in_use(char *path, int max, char *when, int wmax);

#define VLHE_DRAFT_MAX 40
struct vlhe_draft_review {
    int  nchange, nrefused, nskipped, nuser;
    char change[VLHE_DRAFT_MAX][160];
    char refused[VLHE_DRAFT_MAX][140];
    char skipped[VLHE_DRAFT_MAX][90];
};
/* change[] lines are TAB-separated: section, key, old value, new
 * value. refused[] and skipped[] are text. */
int  vlhe_conf_draft_review(const char *path, struct vlhe_draft_review *r,
                            char *why, int max);
int  vlhe_conf_draft_accept(void);
void vlhe_conf_draft_cancel(void);
/* An accepted draft is still unsaved: Load's save-first question asks
 * about it. Cleared by a system-half Save or by discard - R7. */
int  vlhe_conf_draft_pending(void);

/*
 * WHERE THE FILES ARE, and whether this user could write the system
 * one - for the first-run dialog, which names the paths because that
 * is how someone learns they exist.
 *
 * THROUGH THE BACKEND RATHER THAN vlhe_conf.h, so the GUI keeps
 * including one header and nothing else - the seam design/32 calls
 * the point of this layer. The fake backend answers with plausible
 * paths and touches no file.
 */
const char *vlhe_system_conf_path(void);
const char *vlhe_user_conf_path(void);
int         vlhe_can_write_system_conf(void);

/* Did the seed supply the defaults? For the prompt's wording - "with
 * the settings from installation" rather than "with default
 * settings", which are different reassurances and only one is true at
 * a time (design/33 section 1b). */
int vlhe_seed_exists(void);

/* ------------------------------------------------------------------ */
/* Status - every module and daemon, and how to restart them          */
/* ------------------------------------------------------------------ */

/* ONE PAGE FOR ALL SIX, because design/09 asks for a restart control
 * and says it is worth solving ONCE:
 *
 *   "If vmidi wedges... the slot stays claimed and MIDI stays dead
 *    until the module is unloaded, which takes the whole audio path
 *    down with it. THE SAME PROBLEM EXISTS FOR vsoundd AND vdiscd and
 *    is worth solving once."
 *
 * A restart button in each module's own tab would be three places to
 * look for one class of action, and two of them are in modules whose
 * settings have nothing to do with it.
 *
 * AND IT IS THE PAGE THAT MAKES SENSE WHEN THINGS ARE BROKEN. Volume,
 * CD and MIDI all have a "the module is not loaded" state; this is
 * where that fact belongs and where the fix is. Hence first in the
 * sidebar, above Volume.
 *
 * WHAT IT CANNOT DO YET: tell a WEDGED daemon from a quiet one.
 * design/09 says that needs the counters - "a way to SEE that a daemon
 * is wedged rather than merely quiet, which is what the counters are
 * for" - and they do not exist. So `running' means the process is
 * there, not that it is working, and the page must not imply more. */

#define VLHE_MAX_COMPONENTS 8

/*
 * ---- WHICH COMPONENTS ARE ENABLED ------------------------------
 *
 * Three independent settings, one per page, stored as `LoadAtBoot' in
 * each section - a name that predates them meaning anything at
 * runtime, and kept because renaming a config key strands every file
 * already written.
 *
 * NOTHING READ THEM UNTIL 2026-09-22. The keys were in the template,
 * `vlhe-init' grepped the file to decide whether to run at all, and
 * plan_load() ignored them entirely - so a user who turned MIDI off
 * still got vmidi.o loaded by every apply. The per-component choice
 * was designed and half-built; this is the half that was missing.
 *
 * AND "AT BOOT" IS THE WRONG WORD IN THE GUI, which is why the label
 * says `Enable' - the user's point, 2026-09-22: "Load at boot is
 * meaningless for someone in the portable mode they would not want
 * something loading at boot that would confuse them". A portable copy
 * has no init script installed, so nothing of ours loads at boot
 * there and a checkbox promising it would be lying. What the setting
 * really means is "use this component", of which loading at boot is
 * an INSTALLED-ONLY consequence.
 */
#define VLHE_ENABLE_SOUND 0
#define VLHE_ENABLE_MIDI  1
#define VLHE_ENABLE_CD    2

/*
 * IS THIS COMPONENT IN THE CONTROL CENTRE'S LOAD? The Status page's
 * "Include in load" boxes - the `Include' key of each section.
 *
 * NOT THE SAME KEY AS LoadAtBoot SINCE 2026-10-02. They were one key,
 * which the user says was to make testing easier: "Status shouldn't
 * touch boot behaviour." So the GUI's Load reads and writes Include;
 * what the init script does at boot is LoadAtBoot, read by
 * vlhe_component_at_boot() and set by the setup program, never by
 * the Status page. Include DEFAULTS TO LoadAtBoot when absent, so a
 * config from before the split loads exactly as it did.
 */
int  vlhe_component_enabled(int which);

/* Turn one on or off. Memory only - vlhe_commit() writes. */
void vlhe_set_component_enabled(int which, int on);

/* IS THIS COMPONENT LOADED AT BOOT - the LoadAtBoot key, which the
 * init script's `vlhe apply' plans from. Defaults to TRUE when absent,
 * because a config written before these were honoured meant all
 * three.
 *
 * SET BY THE SETUP PROGRAM AND, SINCE 2026-10-08, BY ADVANCED SETTINGS'
 * STARTUP TAB (the user: "add a load on startup option") - still never
 * by the Status page, whose boxes are Include. Memory only; File >
 * Save writes. -1 for anything but 0 or 1. */
int  vlhe_component_at_boot(int which);
int  vlhe_set_component_at_boot(int which, int on);

/* DIAGNOSTIC TRACING - the `[Tracing] Enabled' key, default 0. When
 * set, the load plan passes the modules' trace parameters and starts
 * vmidid with -v (design/09, "TRACING IS DEBUGGING"). 0 off, 1 events,
 * 2 everything. The setters are Advanced Settings' Debugging tab
 * (2026-10-08); memory only, File > Save writes, -1 out of range. */
int  vlhe_tracing(void);
int  vlhe_set_tracing(int level);
/* [Tracing] Capture: the run folder - `vlhe trace' started with the
 * load, writing the modules' rings to run-<stamp>/trace.log beside
 * DAEMON.LOG. The rings are the only place a trace line goes
 * (modules/common/vtrace.h has the history of the kernel-log path that
 * was kept beside them for one day's comparison and then removed). */
int  vlhe_trace_capture(void);
int  vlhe_set_trace_capture(int on);
/* [Boot] FinishLeftover - 1 (the default): `vlhe apply --boot' finishes
 * a load that was never unloaded before loading. design/54 7h. */
int  vlhe_boot_finish_leftover(void);
int  vlhe_set_boot_finish_leftover(int on);   /* memory; File > Save writes */
/* [Load] BaselineDrift - design/54 7h Stage 4: what a Load from the
 * control centre or a terminal does when a path VLHE manages has
 * changed since the baseline was recorded and VLHE did not change it.
 * A MACHINE setting (the system file), set in File / Preferences.
 * "ask" (the default) shows the drift dialog or prompt; "warn" keeps
 * the baseline and loads; "refuse" stops the Load with nothing
 * changed ("abort", design/54's word, reads the same). Deliberately
 * no "accept": the baseline is only ever replaced by someone choosing
 * it. The boot always warns, whatever this says - no one is there. */
enum { VLHE_BDRIFT_ASK = 0, VLHE_BDRIFT_WARN, VLHE_BDRIFT_REFUSE };
int  vlhe_baseline_drift(void);
int  vlhe_set_baseline_drift(int mode);     /* memory; File > Save writes */
/* [CD Settings] DrivesAutoLoad: 1 when vdiscd should reattach, at
 * start, the drives the drive state marks Autoload - the master switch
 * beside the CD page's per-drive "Load at startup" boxes. Default 1. */
int  vlhe_drives_autoload(void);
/* [Midi Settings] ChannelRelease: ms of silence (no voice, effects
 * faded) before vmidid lets its channel go; 0 = hold. Default 3000,
 * clamped to 0..600000; a negative value reads as the default. */
int  vlhe_midi_release_ms(void);
int  vlhe_set_midi_release_ms(int ms);        /* 0..600000, or -1 */
/* [Sound Settings] MixRate: the rate the pump mixes at, or 0 to ask the
 * card. Read for `vsoundd -S' - design/54 D14, 2026-10-03: the key and
 * the flag both existed and nothing joined them. Anything outside
 * 8000..48000 (the mixer's ceiling) reads as 0. */
int  vlhe_mix_rate(void);

#define VLHE_COMP_MODULE 0      /* insmod'd, restart means reload      */
#define VLHE_COMP_DAEMON 1      /* a process, restart means respawn    */

struct vlhe_component {
    char name[VLHE_NAME_MAX];   /* "vsound", "vsoundd"                 */
    int  kind;                  /* VLHE_COMP_*                         */
    int  present;               /* loaded, or running                  */
    int  pid;                   /* daemons only; 0 otherwise           */
    char detail[64];            /* "3 of 5 channels", "minor 15"       */

    /* WHAT CAN BE DONE TO IT FROM HERE.
     *
     * A MODULE GETS NEITHER, AND NO BUTTON AT ALL - the user's call,
     * 2026-09-17: "if they cant be restarted from that page do they
     * need a button at all". A greyed button is a control that exists
     * only to be refused, and the paragraph beneath the list already
     * says why modules are not restarted here. A screen reader
     * announcing "Restart, unavailable" three times is worse than
     * silence.
     *
     * AND THE INIT SCRIPT IS THE REAL MECHANISM. design/09's "HOW THE
     * DAEMONS START" puts start|stop|restart|reload|force-reload in
     * /etc/init.d, modelled on kerneld - so a GUI button would be a
     * second path to the same thing.
     *
     * RESTART IS THE ONLY VERB, and a `can_reload' was tried and
     * dropped. design/09 wants /etc/init.d/skeleton's `reload)' case
     * BUILT - "it would let the GUI apply a config change without
     * stopping audio" - but nothing implements it: vmidid and vdiscd
     * take their settings as command-line arguments parsed once at
     * exec, with no config reread and no signal handler. A reload
     * button would have done nothing. */
    int  can_restart;
};

int vlhe_components(struct vlhe_component *out, int max);

/* Restart one daemon by name - stop it and start it again. Returns 0,
 * or -1 with the reason left to the caller to report - the likely ones
 * being "not running", "needs root" and "it did not come back". */
int vlhe_restart(const char *name);

/*
 * DOES THIS DAEMON CARE WHAT IS ON THAT PAGE? - 2026-09-27.
 *
 * THE USER'S REQUIREMENT: *"if we are resetting vmidid we should only
 * care if vmidid is dirty and not any cd settings etc"*. Restarting
 * the synth must not nag about a half-finished CD change.
 *
 * BUT IT IS NOT ONE PAGE PER DAEMON, WHICH IS WHY THIS FUNCTION
 * EXISTS RATHER THAN A strcmp AT THE CALL SITE. `out_token()'
 * (vlhe_apply.c) decides where a daemon SENDS ITS AUDIO from whether
 * the Sound module is enabled - so Sound Settings feeds the synth and
 * the disc server as well as the pump:
 *
 *     vmidid    Midi Settings, Sound Settings
 *     vdiscd    CD Settings,   Sound Settings
 *     vsoundd   Sound Settings
 *
 * A strictly per-page check would ignore a pending Sound change and
 * restart with the OLD `-o', which looks exactly like the restart
 * having done nothing - the silent-stale-value trap that cost a
 * whole evening on 2026-09-27.
 *
 * `page' is the module title as vlhe_cc.c's table spells it ("Midi
 * Settings"). Returns 1 when a change there would alter how `name'
 * is started, 0 otherwise. An unknown daemon or page answers 0: a
 * false NO costs a stale restart the user can repeat, where a false
 * YES nags about something irrelevant on every press.
 */
int vlhe_restart_wants_page(const char *name, const char *page);

/*
 * ONE DRIVE'S IMAGE PATH, IN FULL - 2026-09-27.
 *
 * `vlhe_drives()' FILLS `image' FROM `status', WHICH TRUNCATES IT
 * TO 28 CHARACTERS. That reply names every drive in one line, so it
 * has to, and the field was only ever DISPLAYED - until the CD+G
 * viewer began opening the image itself.
 *
 * Measured on 86Box: `/mnt/discs/infosoc/infosoc.ccd' reached the
 * GUI as `/mnt/discs/infosoc/infosoc.i', the open failed, and the
 * page showed no picture, no tracks and no CD-TEXT while the disc
 * played normally. So ANYTHING THAT OPENS THE IMAGE MUST ASK HERE;
 * `vlhe_drive.image' remains fine for showing which disc is in.
 *
 * Needs the daemon - there is no state-file fallback, because a
 * path nobody can confirm is worse than saying so.
 *
 * 0 and fills `out', or -1 (no daemon, no such drive, no image).
 */
int vlhe_drive_image(int index, char *out, int max);



/* ------------------------------------------------------------------ */
/* Sound - the pump and its module                                    */
/* ------------------------------------------------------------------ */

/* THE SMALLEST SURFACE OF THE THREE, and the mockup's version of this
 * tab describes none of it.
 *
 * `vsound.o' HAS EIGHT MODULE PARAMETERS AND ONE IS USER-FACING:
 * `vsound_midi', which reserves a channel for the synth. The rest are
 * trace knobs (`vsound_trace', `vsound_ratelimit', the three
 * per-category rates) or tuning with an auto default (`vsound_depth',
 * `vsound_write_ms'), and design/09 keeps those out of the GUI.
 *
 * AND THE CHANNEL COUNT IS NOT A PARAMETER AT ALL. `VSOUND_MAX_CHAN'
 * is 4, a compile-time constant in vsound_chan.h - so the mockup's
 * channel setting, and `design/vsound.conf.example''s "Channels = 4",
 * describe something that needs a RECOMPILE. Recorded as a correction
 * to that example rather than built as a control.
 *
 * `vsoundd' ADDS THREE: the card device, -R, and a debug capture. */

struct vlhe_sound {
    /*
     * THE USER'S PRE-LOAD PICK - the node their speakers are on now,
     * which is the node vsound takes. NOT the card's node afterwards:
     * that is derived by the plan and is what `vsoundd -d' gets.
     * Empty means detect. design/38.
     */
    char card[VLHE_PATH_MAX];

    /*
     * THE DEVICE THE USER'S PROGRAMS OPEN, pointed at vsound while
     * loaded. "/dev/dsp" unless they say otherwise - see the
     * template. A DIFFERENT QUESTION FROM `card' above: which name
     * software opens, against which card to play through.
     */
    char programs_use[VLHE_PATH_MAX];
    /*
     * THE CARD'S OWN MODULE, AND WHY IT IS A STRING THE USER CAN SET.
     *
     * vsound has to register at DEVICE 0 or it does not get
     * /dev/dsp, and a legacy driver already resident at boot holds
     * that slot - CLAUDE.md section 5: "vcdsnd must load before the
     * user's sound card driver, or applications bind to the real
     * card and silently bypass the mixer". So applying a config has
     * to move that driver aside and put it back afterwards, exactly
     * as vsound/tests/load.sh's card_check_free_legacy() does.
     *
     * WE CANNOT GUESS THE NAME. The user, 2026-09-21: "a user may be
     * using a sound module that they built themselves that we wont
     * know about". A built-in list only ever finds cards we thought
     * to list - vlhe_status.h:62 makes the same argument for
     * DETECTION and answers it by asking soundcore who holds it.
     * Detection is the default here for the same reason; this field
     * is the OVERRIDE, so a hand-built driver can be named.
     *
     * EMPTY MEANS DETECT AT APPLY TIME. It does not mean "no card".
     *
     * AND AN UNKNOWN HOLDER IS REFUSED, NOT UNLOADED. load.sh:394
     * has the precedent and the reason: "Unloading whatever happens
     * to be there is a different and much less safe thing than
     * unloading the one driver this script knows about."
     */
    char card_module[64];           /* "sb"; empty = detect           */
    int  release_on_idle;           /* -R                              */
    int  midi_slot;                 /* vsound_midi, module parameter   */
    int  midi_slot_applied;         /* what the running module has     */
    int  channels;                  /* VSOUND_MAX_CHAN - READ ONLY     */
    int  loaded;
    int  running;                   /* the pump                        */
    /* AT THE END so positional initialisers keep their meaning. */
    int  limiter;                   /* vsound_limit: 0 off, 1 attack/
                                     * release, 2 soft knee            */
    int  attenuation;               /* vsound_atten, percent, 1-100    */
};

int vlhe_sound(struct vlhe_sound *out);
int vlhe_set_sound(const struct vlhe_sound *in);

/* The sound cards this machine has, for the device menu. A machine
 * with one card gets one entry and the menu is a formality; a machine
 * with two needs to be told which is the real one. */
struct vlhe_card {
    char node[VLHE_PATH_MAX];       /* "/dev/dsp1"                     */
    char name[64];                  /* "ESS Solo-1", from /proc/sound  */
    int  is_ours;                   /* is this vsound's own device     */
    int  busy;                      /* open refused: someone holds it  */
};

/*
 * HOW MANY /dev/dspN TO PROBE, AND IT IS A BUILD-TIME SETTING -
 * -DVLHE_DSP_PROBE_MAX=N overrides it. The user's call 2026-09-24.
 *
 * THE LIMIT IS NOT THE CARD COUNT. es1371 and es1370 register TWO dsp
 * minors each - dev_audio and dev_dac (es1371.c:3127 and :3131) - so
 * two Ensoniq cards fill four slots on their own, before vsound takes
 * one. Every other driver in 2.2.16 takes one: cmpci, esssolo1,
 * sonicvibes, trident, maestro, i810_audio, sb.
 *
 * FOUR WAS ENOUGH FOR A STOCK MACHINE AND WAS NOT ENOUGH FOR OURS -
 * see the correction at the end. `MAKEDEV audio' creates exactly TWO
 * dsp nodes - /dev/dsp (minor 3) and /dev/dsp1 (19), hardcoded,
 * whatever hardware is present - so a stock install cannot reach a
 * third card at all until something makes the node. That reasoning
 * is still true and it answered the wrong question: the setting
 * bounds what the GUI will SHOW, and every machine in this project
 * has sixteen nodes because the lineage ran the paid OSS.
 *
 * THE PAID OSS IS WHAT MAKES THE REST, measured 2026-09-24 by running
 * `oss_setup' on a fresh install and watching them appear: dsp0 and
 * dsp2..dsp15, the whole dspW set and dspdefault, all `root:root'
 * where MAKEDEV's two are `root:audio'. The ownership is the
 * fingerprint, and it is why every machine in this project has
 * sixteen - the lineage ran it. `sndconfig' does NOT do this.
 *
 * THE KERNEL'S OWN CEILING IS 8, NOT 16 - CORRECTED 2026-09-25 AND
 * MEASURED ON TARGET. This comment said 16, reasoning from the
 * sixteen `/dev/dspN' nodes MAKEDEV and the paid OSS create. The
 * kernel hands out far fewer:
 *
 *     register_sound_dsp() -> sound_insert_unit(&chains[3], fops,
 *                                               dev, 3, 131)
 *
 * `top = 131' with a step of 16 means the last minor it will ever
 * allocate is 115, which is `/dev/dsp7'. Minors 131..243
 * (`dsp8'..`dsp15') are valid device numbers that `soundcore' will
 * NEVER hand out. Identical in 2.2.0, 2.2.16 and 2.2.26.
 *
 * PROVEN BY FILLING IT: four Ensoniq cards at two dsp minors each
 * take all eight slots, and `insmod vsound.o' then FAILS rc=1 -
 * `tests/logs/2026-09-25-four-cards-dsp-ceiling/'. There is no
 * ninth slot to land in.
 *
 * THE PAID OSS REACHES 16 BY NOT USING THIS API AT ALL. Its
 * `soundbase' module imports `module_register_chrdev' and claims
 * major 14 itself, with its own limits - it does not export
 * `register_sound_dsp', so nothing of ours can borrow that ceiling
 * without being ported to `sound_install_audiodrv'. design/40 8c.
 *
 * SO 8 IS THE REAL CEILING FOR THIS SETTING, AND IT IS NOW 8 -
 * raised 2026-09-26, design/36 row 81.
 *
 * WHAT IT FIXES, seen on target 2026-09-25: on a guest with four
 * Ensoniq cards the user reported *"strangely playthrough and the
 * other only show dsp, dsp1, dsp2 and dsp3 no more"*. Eight devices
 * were present and the GUI could list half of them - both the Play
 * through and Programs use menus stopped at the fourth candidate.
 *
 * WHAT IT COSTS, AND ROW 81's OWN ESTIMATE WAS WRONG. That row
 * warned the Device page would cost "eight opens instead of four
 * EACH TIME IT IS DRAWN" and asked for a measurement on the P1.
 * There is nothing to measure: `vlhe_cards()' is called from
 * `build_device()', a page CONSTRUCTOR, and `vlhe_cc.c:3798' builds
 * every module once in a startup loop - pages are shown and hidden
 * on selection, never rebuilt, deliberately (`vlhe_cc.c:203`, so a
 * half-typed entry is not lost). So the cost is FOUR EXTRA `open()'
 * CALLS, ONCE, at launch.
 *
 * AND CACHING WOULD BE WRONG, considered and rejected the same day.
 * The GUI's answer is already built once into the option menus. The
 * other users of this bound - `probe_vsound()' and `probe_card()' in
 * `vlhe_apply.c' - run DURING a load and exist to see the machine as
 * it is after modules were just inserted, so a cached answer there
 * would be stale by design.
 *
 * IT ALSO SIZES `g_use_node[VLHE_DSP_PROBE_MAX][VLHE_PATH_MAX]'
 * (`vlhe_mod_sound.c:76`), so the change costs a kilobyte of static
 * data as well as the four opens.
 */
#ifndef VLHE_DSP_PROBE_MAX
#define VLHE_DSP_PROBE_MAX 8
#endif

#define VLHE_MAX_CARDS  8
int vlhe_cards(struct vlhe_card *out, int max);

/*
 * THE SOUND DRIVERS LOADED ON THIS MACHINE, BY MODULE NAME.
 *
 * DIFFERENT FROM vlhe_cards() AND FOR A DIFFERENT JOB. That one
 * probes /dev/dsp..dsp3 and answers "which DEVICE do I play through";
 * this answers "which MODULE is the user's card", which is what the
 * load order needs - vsound must be inserted BEFORE the card's driver
 * or applications bind to the card and bypass the mixer (CLAUDE.md
 * section 5), and doing that to a running machine means unloading the
 * card, loading vsound, and loading the card again.
 *
 * IT READS soundcore's BRACKET LIST, and that is the only method that
 * finds every case - see vlhe_status_module_holders(). Our own
 * modules are filtered out, so what is left is the user's.
 *
 * NOT HARDWARE DETECTION. It reports what is LOADED. A machine with
 * the wrong driver loaded reports the wrong driver, and a card whose
 * driver is not loaded does not appear at all - which is correct,
 * since there would be nothing for us to sit in front of.
 *
 * SO THE GUI MUST LET THE USER CONFIRM OR OVERRIDE IT. A dropdown
 * with the detected names and room to type one we did not find is
 * what covers an out-of-tree or hand-built driver, and the user asked
 * for exactly that.
 */
struct vlhe_driver {
    char module[64];                /* "es1371", "esssolo1", ...       */
    int  users;                     /* its own use count, or -1        */
    /*
     * THE ARGUMENTS IT WAS LOADED WITH, as far as we can tell - the
     * text after the module name on its /etc/modules line, empty if
     * there is none.
     *
     * REUSING THEM IS NOT OPTIONAL FOR A RELOAD. A legacy ISA card
     * cannot be probed and simply fails without io/irq/dma -
     * CLAUDE.md records `insmod sb' ALWAYS failing bare, where 86Box
     * wants io=0x220 irq=5 dma=1 dma16=5. And a PCI card, which
     * needs no addressing, may still carry OPTIONS that matter: the
     * user's point, 2026-09-21 - es1371 takes `joystick' and `spdif'
     * (es1371.c's MODULE_PARM), so reloading it bare would silently
     * take away a joystick that was working.
     *
     * WE NEVER INVENT THESE. If the machine booted with that card
     * working, its /etc/modules arguments are correct by
     * demonstration; anything we made up would not be.
     *
     * AND THEY MAY BE ABSENT WITHOUT THE CARD BEING MISCONFIGURED.
     * sndconfig writes `alias sound es1371' into /etc/conf.modules
     * and nothing into /etc/modules (CLAUDE.md section 3), and a
     * driver loaded by hand after boot is named in neither file. So
     * empty means "we did not find any", never "there are none" -
     * which is why the GUI must let the user type them.
     */
    char args[128];
};

#define VLHE_MAX_DRIVERS 8
int vlhe_sound_drivers(struct vlhe_driver *out, int max);

/* ------------------------------------------------------------------ */
/* MIDI - the synth daemon and its module                             */
/* ------------------------------------------------------------------ */

/* THE SPLIT IS NOT THE SAME AS THE CD's, and the tabs follow it.
 *
 * vmidi.o (the MODULE) has exactly ONE user-facing parameter -
 * `vmidi_minor', the sound minor for /dev/vmidi. Its other three are
 * trace knobs, which design/09 keeps out of the GUI.
 *
 * vmidid (the DAEMON) has everything else: the fonts, the voice cap,
 * gain, the volume law, the velocity filter, effects and rate.
 *
 * So MIDI Options is mostly the DAEMON where CD Options was mostly the
 * MODULE - and that inverts the privilege picture. The synth settings
 * need no root; the minor needs root AND a module reload. Mixing them
 * on one page would grey one control in six, which reads as broken
 * rather than as a distinction, so the minor gets its own tab. */

#define VLHE_MAX_FONTS  8       /* RENDER_MAX_FONTS */
/* vmidid's VMIDID_MAX_VOICES, which the build passes to every compile
 * (defs.mk, `make MAX_VOICES='); 512 when it does not. Was 64, the
 * fixed array's size, until the array became allocated 2026-10-02. */
#ifdef VMIDID_MAX_VOICES
#define VLHE_MAX_VOICES VMIDID_MAX_VOICES
#else
#define VLHE_MAX_VOICES 512
#endif

/* ONE FONT IN THE STACK.
 *
 * FONTS STACK, THEY DO NOT REPLACE - which is why this is a list and
 * not a chooser. vmidid's -s repeats: "-s gm.sf2 -s song.sf2@1 puts
 * that font's bank 0 in MIDI bank 1; LATER FONTS ARE SEARCHED FIRST."
 * So order is meaningful and the GUI must let it be changed.
 *
 * The mockup called this tab "Patch Banks" with .bnk files. Nothing
 * here reads .bnk; the format is SoundFont 2. */
/* ONE FONT IN THE STACK - and in practice there are at most two.
 *
 * THE LIST WAS REWORKED INTO TWO SLOTS, 2026-09-17, at the user's
 * call: "Most times a single font is required not two... The
 * confusing part is the last one searched first."
 *
 * THE IDIOM IS FIXED, not arbitrary layering. render.h:493-499 has it
 * from the corpus: "the AWE32-era songs
 * (vmidi/refs/awe32-midi-conversions/, 106 OF 112) are written for
 * exactly that - A GM FONT IN BANK 0, THE SONG'S OWN IN BANK 1."
 *
 * So the general case is one font, the interesting case is two, and
 * nothing in 112 real songs wants three. "Later fonts are searched
 * first" only decides anything when two fonts claim the SAME bank,
 * which in that idiom never happens - so the rule was an explanation
 * of a case users do not hit.
 *
 * Three or more, and banks above 1, stay reachable from vmidid's
 * command line for anyone who needs them. */
struct vlhe_font {
    char path[VLHE_PATH_MAX];
    int  bank;                  /* 0 = general MIDI, 1 = the song's    */
    int  ok;                    /* did vmidid manage to load it        */
};

/* THE USER'S OWN FONTS - [Midi Settings] Font0/Font1 in the user file.
 * What the Midi page shows and edits. */
int vlhe_fonts(struct vlhe_font *out, int max);
/* Set them, memory-only. WHEN THE REAL USER IS ROOT this also sets the
 * MACHINE DEFAULT (DefaultFont0/1 in the system file) - design/54 D15,
 * the user 2026-10-03: "Root saving fonts in the gui writes them as the
 * machine defaults". */
int vlhe_set_fonts(const struct vlhe_font *in, int n);

/*
 * THE FONTS THAT WILL LOAD - design/54 D15, 2026-10-03. The user's own
 * if they have any, else the machine's default from /etc/vlhe.conf
 * ([Midi Settings] DefaultFont0/DefaultFont1). The plan, Simulate, the
 * Status page and Render use this; the Midi page uses vlhe_fonts().
 *
 * WHY: the boot-time `vlhe apply' runs from init, and Linux 2.2 starts
 * init with HOME=/ (init/main.c:430) - so the user file it finds is
 * /.vlhe/vlhe.conf, not root's, and with user fonts only it planned
 * vmidid as "no font configured" on every boot. The machine default is
 * what boot plays. Per-user fonts on a running synth are the login
 * agent's job (`set font', design/33 3k step 2, design/54 P01): this
 * keeps the default for boot and leaves the user's choice to be SENT.
 *
 * `*from_machine' (may be NULL) is set to 1 when the default was used.
 */
int vlhe_fonts_effective(struct vlhe_font *out, int max, int *from_machine);

/* FOR THE HOST TESTS: treat the real user as root (1), not (0), or ask
 * getuid() (-1, the default). */
void vlhe_backend_test_root(int is_root);

/* ------------------------------------------------------------------ *
 * Resetting settings to their defaults
 * ------------------------------------------------------------------ *
 *
 * WHAT `DEFAULT' MEANS HERE: the SEED file when one exists
 * (/etc/vlhe/defaults.conf - design/33 section 1b), and otherwise
 * the compiled-in template. A vendor or an administrator who has
 * placed a seed has said what a fresh machine should look like, and
 * a reset should return to THAT rather than to what we shipped.
 *
 * EVERY RESET BACKS UP FIRST, to `<path>.old', and the caller is
 * expected to show the user where. That turns the warning from "this
 * cannot be undone" into "here is how to undo it", which is both
 * truer and calmer - and vlhe_conf_write() already keeps a .old, so
 * the mechanism is the one the rest of the program uses.
 *
 * THE SCOPES, and the third and fourth need root:
 */
#define VLHE_RESET_USER     1   /* this user's ~/.vlhe/vlhe.conf      */
#define VLHE_RESET_SYSTEM   2   /* /etc/vlhe.conf                     */
#define VLHE_RESET_ALLUSERS 4   /* every home directory's copy        */

/*
 * Reset one or more scopes - the flags are a bitmask, so "everything"
 * is all three ORed together.
 *
 * `report' is called once per file with what happened to it, for a
 * GUI that wants to list them; NULL for silence. Returns the number
 * of files reset, or -1 if a scope was refused outright.
 *
 * ALLUSERS WALKS /home AS ROOT, which is a thing to be deliberate
 * about: it touches files belonging to people who are not at the
 * keyboard. It is refused when not running as root, and it is the
 * same code an uninstaller's `postrm purge' wants - dpkg tracks only
 * conffiles the PACKAGE shipped, and neither /etc/vlhe.conf (written
 * by setup-vlhe at runtime) nor a user's ~/.vlhe/vlhe.conf is one,
 * so purge would otherwise leave both.
 */
int vlhe_reset_settings(int scopes,
                        void (*report)(const char *line));

/*
 * THE ALLUSERS SCOPE ITSELF, without the root check above - for the
 * host test. Each file is written by a child that has become the
 * account owning that home (design/55 recommendation 5), so run
 * without root it can reset only the caller's own; anyone else's is
 * reported as "could not become its owner" and left alone. Returns
 * the number reset. Call vlhe_reset_settings() everywhere else.
 */
int vlhe_reset_all_users(void (*report)(const char *line));

/* Is this process root? The GUI greys the scopes that need it.
 *
 * NOTE THE TENSE: this is "is euid 0 NOW". In the setuid build after
 * Modify, euid is 0 only inside a vlhe_root_begin()/end() window, so
 * a GUI deciding whether to OFFER a privileged action asks
 * vlhe_priv_can_act() instead (vlhe_priv.h). Inside a window - where
 * the backend itself calls this - the answer is the same as before. */
int vlhe_is_root(void);

/* ARE THIS ACCOUNT'S FONTS THE MACHINE'S? The REAL user's identity -
 * what vlhe_set_fonts() goes by when it mirrors root's choice into
 * DefaultFont* (D15) - so the Sound Fonts page can say "yours to
 * change" to root and "choose your own" to anyone else. The setuid
 * build is euid 0 around its privileged work while the person at the
 * desk is not root; this answers for the person. */
int vlhe_fonts_as_root(void);

/*
 * ---- RAISING PRIVILEGE AROUND THE WORK THAT NEEDS IT ------------
 *
 * design/48 S4: after Modify the setuid build used to keep euid 0 for
 * the rest of the session, so GTK, the polls, Render and LAME all ran
 * as root though none needed it. Now euid is 0 only around the work
 * that does need it, and these mark that work.
 *
 * THEY CALL HOOKS THE GUI REGISTERS, which is why they live here and
 * not in vlhe_priv.c. The CLI, vlhe-probe and the host tests link
 * the backend but not vlhe_priv.c; for them no hooks are set, these
 * do nothing, and behaviour is exactly as before.
 *
 * WHAT IS WRAPPED, and anything that needs root and is NOT on this
 * list stops working in the setuid build after Modify:
 *
 *   - every vdiscd and vmidid control-channel request. The FIFOs are
 *     mode 0660 under /var/run/vlhe, so attach, detach, autoload and
 *     the CD+G transport need root to reach the daemon
 *   - the SYSTEM half of vlhe_commit() - /etc/vlhe.conf
 *   - vlhe_reset_settings()
 *   - vlhe_plan_run() and vlhe_restart(), around themselves
 *
 * NESTING IS SAFE: begin/end count, and only the outermost end
 * lowers. begin returns -1 if privilege could not be raised; the
 * caller proceeds anyway and the operation fails as it would for an
 * unprivileged user, which is the correct degraded behaviour.
 */
void vlhe_backend_set_priv(int (*raise)(void), void (*lower)(void));
int  vlhe_root_begin(void);
void vlhe_root_end(void);

/*
 * ---- THE PORTABLE COPY'S FIRST RUN ----------------------------
 *
 * SHOULD THE USER BE ASKED ABOUT THEIR INSTALLED SETTINGS?
 *
 * True only when all three hold:
 *
 *   - this is a portable copy (the PORTABLE marker beside the binary)
 *   - the machine HAS an installed config, so there is something to
 *     copy
 *   - the tree has NO config yet, so nothing would be overwritten and
 *     the question has not already been answered
 *
 * THE THIRD IS WHAT MAKES IT ONCE. After the first run the tree has
 * its own config and this is false forever after - asking every
 * launch would be noise, and asking after edits would risk throwing
 * them away.
 *
 * THE CASE IT SERVES is the user's own, 2026-09-22: someone with VLHE
 * installed who unpacks a newer build "to try it out to see if it
 * addresses any issues they have". They almost certainly want THEIR
 * settings in the new copy rather than defaults - but it is their
 * call, which is why this asks rather than deciding.
 */
int vlhe_portable_offer_seed(void);

/*
 * COPY THE INSTALLED CONFIG INTO THIS PORTABLE TREE.
 *
 * A COPY, so nothing installed is touched - which is the promise
 * portable mode makes and the reason this is safe to offer at all.
 * The system config is copied to the tree's; a user config is copied
 * too when one exists.
 *
 * Returns the number of files copied, or -1 on failure with a reason
 * in `why'. Zero is not a failure: it means there was nothing to
 * copy, which the caller reads as "start with defaults".
 */
int vlhe_portable_seed(char *why, int max);

/*
 * WHERE THE INSTALLER PUT THE MODULES - `[Paths] ModuleDir', or ""
 * when the key is absent, which means this machine has never been
 * installed. See that key's comment in vlhe_conf_template.c for why
 * the installer writing it beats anything probing for it.
 *
 * Returns a static buffer, never NULL.
 */
const char *vlhe_module_dir_conf(void);

/* ------------------------------------------------------------------ *
 * Rendering a MIDI file - the Render MIDI page's Advanced tab
 * ------------------------------------------------------------------ *
 *
 * SEPARATE FROM struct vlhe_synth, AND THAT IS THE POINT. The synth
 * has a deadline and these do not, so the two disagree about what a
 * sensible voice count is by a factor of thirty-two: `Midi Settings'
 * caps at 64 because vmidid has to keep up, and a render can have
 * 2048 because it does not.
 *
 * PER-USER, not per-machine. These describe how one person likes
 * their renders; nothing here reaches the daemon or changes what the
 * machine plays, which is what the page says on it.
 *
 * `use_vlhe' DECIDES WHICH SET A RENDER USES - with it set, the rest
 * of this structure is ignored and the render takes the Midi
 * Settings. It does NOT disable the controls (the user, 2026-09-22):
 * they are saved settings in their own right and editing them should
 * not need a switch on another tab flipped first.
 */
struct vlhe_render {
    int  use_vlhe;              /* 1 = render as the machine plays    */
    int  voices;                /* 1..2048 - NOT the synth's 64       */
    int  rate;
    int  gain_milli;            /* 1..2000, 1000 = unity              */
    int  law;                   /* VLHE_LAW_*                         */
    int  filter;                /* VLHE_VF_*                          */
    int  reverb;                /* reverb on - `chorus' is at the end */
    int  bypass;                /* skip a low-pass that does nothing  */
    char gm_font[VLHE_PATH_MAX];   /* empty = the Midi Settings' own  */
    char song_font[VLHE_PATH_MAX]; /* empty = none                    */
    /*
     * WHERE LAME IS. Remembered because Corel ships none, so the
     * path is whatever the user built or fetched - and retyping it
     * every session is the kind of friction that makes a feature go
     * unused. The user, 2026-09-23: "The lame path should also be
     * saved so I dont have to type it each time".
     *
     * EMPTY MEANS "NOT SET", and the page then looks beside itself in
     * a portable tree before giving up - the same Tools= lookup
     * smf2wav uses.
     */
    char lame[VLHE_PATH_MAX];
    /*
     * WHERE THE TEMPORARY WAV GOES during an MP3 render - the WAV is
     * ten times the MP3, and beside the output it competed with the
     * MP3 for a small transfer volume (the user, 2026-10-02, having
     * filled one). VLHE_WAVTEMP_AUTO picks whichever of /tmp and the
     * output's folder has more room; the other two are the user's
     * override - "Automatic by default. and a user can override it".
     */
    int  wav_temp;              /* VLHE_WAVTEMP_*                     */
    int  modenv;                /* VLHE_MODENV_* - smf2wav -M, D28    */
    /* REVERB AND CHORUS APART since 2026-10-05; at the END so the
     * positional initialisers keep their meaning. */
    int  chorus;
};

#define VLHE_WAVTEMP_AUTO   0   /* the roomier of /tmp and the output's folder */
#define VLHE_WAVTEMP_TMP    1   /* always /tmp                                  */
#define VLHE_WAVTEMP_BESIDE 2   /* always beside the output                     */

/*
 * THE OFFLINE RENDERER'S CEILING, AND IT IS A COPY.
 *
 * THE SOURCE IS `vmidi/synth/Makefile', `OFFLINE_VOICES = 2048',
 * which reaches smf2wav as -DRENDER_MAX_VOICES on its own build line.
 * The GUI cannot include that - it is a build variable, not a header
 * - so this is a second number that must be kept level with it, and
 * `vlhe_mod_render.c' holds a THIRD for its spin button.
 *
 * RAISING OFFLINE_VOICES ALONE CHANGES ONLY WHAT smf2wav ACCEPTS.
 * The spinner would still stop here and this validator would still
 * refuse above it, with nothing saying why. Recorded 2026-09-23 after
 * the user asked whether a raise had made it into the GUI.
 *
 * WHY 2048 rather than something smaller: an offline render has no
 * deadline, so the ceiling is headroom rather than a budget.
 */
#define VLHE_RENDER_VOICES_MAX 2048

int vlhe_render(struct vlhe_render *out);
int vlhe_set_render(const struct vlhe_render *in);

/* ------------------------------------------------------------------ */
/* Where SoundFonts are found                                         */
/* ------------------------------------------------------------------ */

/* THREE DEFAULTS, AND THE FIRST IS AWESFX'S OWN.
 *
 *   /usr/share/sounds/sf2          awesfx's DEFAULT_SF_PATH, first
 *                                  entry (awesfx-0.5.0/configure.in:27)
 *   /usr/local/share/sounds/sf2    the source-install equivalent, and
 *                                  design/31 says this release builds
 *                                  on target
 *   ~/.vlhe/sf2                    the user's own, needing no root
 *
 * AWESFX IS THE ONLY CONVENTION THIS PLATFORM HAS - Takashi Iwai's
 * tools, same author as the kernel's awe_wave.c - and its full path is
 * "/usr/share/sounds/sf2:/usr/share/sfbank:/usr/local/lib/sfbank".
 *
 * THE OTHER TWO ARE DELIBERATELY NOT SCANNED. `sfbank' predates .sf2
 * as an extension and was where fonts went for `sfxload' to push into
 * an AWE card's onboard memory. Nobody here has that card, and a path
 * that is always empty still costs a stat, a line in the "no fonts
 * found" message naming somewhere the user should not look, and a
 * puzzle for the next reader.
 *
 * AND NOTHING ON COREL INSTALLS A SOUNDFONT. No package ships one;
 * awesfx only says where to LOOK. So all three directories are empty
 * on a fresh machine and the GUI must say where to put a font rather
 * than showing an empty list.
 *
 * USER PATHS ARE SEARCHED FIRST, so a font in a directory someone
 * added deliberately wins over one of the same name in a system
 * directory. */
#define VLHE_MAX_FONTDIRS 8

int vlhe_font_dirs(char out[][VLHE_PATH_MAX], int max);

/* Add or remove a search directory. Persisted in the config; the
 * defaults cannot be removed, only added to. */
/*
 * RETURNS 0, OR:  -1 the list is full / no user config
 *                 -2 already listed (or one of the built-in defaults)
 *                 -3 no soundfont in that folder
 * The last two were added 2026-09-24 (design/36 row 56) - the GUI's
 * comment said the backend refused a duplicate, and it appended
 * anything, so `./' and `../' from the picker added a folder twice
 * and a parent directory with nothing in it.
 */
int vlhe_add_font_dir(const char *path);
int vlhe_remove_font_dir(const char *path);

/* Every .sf2 found across all of them, deduplicated by BASENAME with
 * the earliest path winning - so a user's own copy of FluidR3 hides
 * the system one rather than appearing twice. */
int vlhe_available_fonts(char out[][VLHE_PATH_MAX], int max);
#define VLHE_MAX_AVAIL  64

/* The synth's runtime settings - all vmidid's, none needing root. */
struct vlhe_synth {
    int  voices;                /* 1..VLHE_MAX_VOICES (512)           */
    int  gain_milli;            /* 1..2000, 1000 = unity, 500 default
                                 * INTEGER because design/07 forbids
                                 * float in anything that crosses a
                                 * wire or a config line              */
    int  law;                   /* VLHE_LAW_*                          */
    int  filter;                /* VLHE_VF_*                           */
    int  reverb;                /* reverb on - `chorus' is at the end  */
    int  rate;                  /* 44100 and friends                   */
    int  running;               /* is vmidid there at all              */
    /* AT THE END so positional initialisers keep their meaning. */
    int  auto_voices;           /* -A: automatic voice reduction,
                                 * design/21 16 - on by default       */
    /* [Midi Settings] ModEnv - design/54 D28: VLHE_MODENV_REFERENCE
     * steps every voice's modulation envelope as the spec does,
     * VLHE_MODENV_FAST skips the unused ones and catches up exactly.
     * The same sound; fast is less work. vmidid -M. */
    int  modenv;
    /* [Midi Settings] Chorus - its own switch since 2026-10-05 (it was
     * written as a copy of Reverb). AT THE END, as above. */
    int  chorus;
};

/* THE -E WORD FOR A REVERB/CHORUS PAIR - vmidid's and smf2wav's `on',
 * `off', `reverb' or `chorus'. A macro so the real and the fake backend
 * builds both have it without a link-time function. */
#define VLHE_FX_WORD(rev, cho) \
    ((rev) && (cho) ? "on" : (rev) ? "reverb" : (cho) ? "chorus" : "off")

#define VLHE_MODENV_REFERENCE 0
#define VLHE_MODENV_FAST      1

#define VLHE_LAW_SPEC   0       /* (v/127)^2 - fluidsynth, the default */
#define VLHE_LAW_LINEAR 1       /* v/127 - the AWE32's old law, ~OPL3  */

#define VLHE_VF_AWE     0
#define VLHE_VF_201     1
#define VLHE_VF_204     2
#define VLHE_VF_NONE    3

int vlhe_synth(struct vlhe_synth *out);
int vlhe_set_synth(const struct vlhe_synth *in);

/*
 * MAKE A RUNNING vmidid MATCH THE SAVED SETTINGS - design/43 6b.
 *
 * What the Status page's "Apply settings" button calls. Sends the
 * saved config down vmidid's control channel, so a change takes
 * effect WITHOUT the stop-and-start that drops the channel and
 * silences whatever is playing.
 *
 *   >= 0   how many settings the daemon REFUSED (0 is success)
 *   -1     the daemon could not be reached at all
 *
 * THE CALLER MUST DISTINGUISH THOSE. "vmidid is not running" and
 * "vmidid said no to two of them" want different messages.
 *
 * AND ONE SETTING DOES NOT TAKE EFFECT AT ONCE: the rate is
 * DEFERRED to the daemon's next release - 3 s of silence by
 * default - because applying it mid-piece would discard the reverb
 * tail and leave sounding voices at the old rate. design/43 6b3
 * says the GUI must show which settings wait; this is the only one.
 */
int vlhe_apply_synth(void);

/* vmidi.o's own parameter. Same shape as the CD's major: a number
 * whose wrong value breaks somebody's card. 15 collides with
 * esssolo1, cmpci and sonicvibes - the module says so itself. */
struct vlhe_midiopts {
    int  minor;
    int  minor_applied;
    int  loaded;
    /*
     * WHICH SEQUENCER MIDI DEVICE vmidi IS - `N' in /proc/sound's
     * "Midi devices:" list, the number lxdoom/musserv want as `-u N'.
     * -1 when vmidi is not loaded or the list cannot be read. Added
     * 2026-09-24, design/36 row 62: with a card loaded there are two,
     * numbered in load order, and nothing had ever reported ours.
     */
    int  seqdev;
};

int vlhe_midiopts(struct vlhe_midiopts *out);
int vlhe_set_midiopts(const struct vlhe_midiopts *in);

/*
 * WHICH MINORS vmidi CAN LIVE ON - read from sound_core.c and every
 * driver in the target tree, 2026-10-01, after a user asked "how do we
 * know 11 is free?". A sound minor is slot*16 + unit, and the UNIT (the
 * low four bits) is what the sound core routes on:
 *
 *   0 mixer, 2 midi, 3 dsp, 9 synth   a card's own chains - collide
 *   1 sequencer, 6 sndstat, 8 seq2    sound.o's, which vmidi needs
 *   4 audio, 5 dsp16                  ALIASES of 3 at open - a vmidi
 *                                     there could never be opened
 *   7, 10, 11, 12, 13, 14             nobody registers them - FREE
 *   15                                esssolo1, cmpci, sonicvibes (dmfm)
 *
 * The menu offers the six free units of slot 0. The setter accepts
 * any slot's free unit (a hand-edited 27 is unit 11 of slot 1), and
 * 15 only while none of the three drivers is loaded or listed in
 * /etc/modules - which is deliberately NOT on the menu: six that work
 * everywhere, against one that stops working the day an ESS or
 * C-Media card is fitted.
 */
#define VLHE_MIDI_MINOR_FREE  "\007\012\013\014\015\016"   /* 7 10 11 12 13 14 */
int vlhe_midi_minor_ok(int minor, char *why, int max);   /* 1 ok, 0 with why */

/* RESTART THE SYNTH WITHOUT UNLOADING THE MODULE.
 *
 * design/09 asks for this: "If vmidi wedges - a synth bug, a patch
 * file it cannot parse, a client feeding it nonsense - the slot stays
 * claimed and MIDI stays dead until the module is unloaded, WHICH
 * TAKES THE WHOLE AUDIO PATH DOWN WITH IT."
 *
 * The same problem exists for vsoundd and vdiscd and is worth solving
 * once; this is the first of the three. O_EXCL makes re-claiming the
 * slot safe - a restarted vmidid opens with the flag and nothing else
 * ever does, so the slot is there whether the old daemon closed
 * cleanly or died holding it. */
int vlhe_restart_synth(void);

/* ------------------------------------------------------------------ */
/* The program's own preferences - File / Preferences                 */
/* ------------------------------------------------------------------ */

/* THE INTERFACE FONT, and this is the ONE setting that is about the
 * program rather than the emulated hardware - which is why it lives
 * in the File menu and not in the sidebar.
 *
 * WHY OFFER IT AT ALL, when an application overriding the desktop's
 * font normally looks foreign: COREL EXPRESSED NO PREFERENCE. The
 * theme's gtkrc is empty ("# Empty gtkrc for default theme"), so GTK
 * falls back to its compiled-in default - Helvetica 12, the pattern
 * in libgtk-1.2.so. And KDE's font panel governs KDE applications,
 * not GTK ones, so there is no system setting to inherit or fight.
 *
 * THE DEFAULT IS "" MEANING "LEAVE GTK ALONE". An empty family is not
 * "Helvetica 12" written out - it is the absence of an override, so a
 * user who later sets a real gtkrc gets what they asked for. */
/*
 * ONE KEY, A FULL XLFD - 2026-09-29, replacing `font_family' and
 * `font_size'.
 *
 * WHY THE PAIR WENT. The GUI now picks fonts with GtkFontSelection,
 * the toolkit's own widget, which returns a complete XLFD:
 *
 *     -adobe-helvetica-medium-r-normal-*-12-*-*-*-p-*-iso8859-1
 *
 * The size is FIELD 7 of that string, so a separate `FontSize' is a
 * second place to say the same thing - and two places that can
 * disagree will. A config reading `FontSize = 10' while the font
 * renders at 12 is exactly the kind of thing that costs an hour.
 *
 * AND A FAMILY NAME ALONE WAS NEVER ENOUGH. The old field held
 * `helvetica' and the GUI rebuilt a pattern around it with a
 * hardcoded `-*-' foundry and `medium-r' weight. That loses what
 * the font actually is: `-urw-urw gothic l-book-r-' comes back as
 * `-*-avantgarde-medium-r-', the same family in a different face.
 * Three attempts at working round it on 2026-09-29 each fixed the
 * last one's bug; the XLFD carries the answer and the toolkit
 * already knew how to produce one.
 *
 * 256 BECAUSE AN XLFD CAN BE LONG. The fourteen fields include a
 * family name with spaces ("new century schoolbook") and a foundry;
 * VLHE_NAME_MAX at 32 would truncate one into nonsense.
 */
#define VLHE_FONT_MAX   256

struct vlhe_prefs {
    char font[VLHE_FONT_MAX];           /* "" = GTK's own default    */
    /*
     * WHICH PAGE THE WINDOW OPENS ON, and where OK returns to.
     *
     * SHIPPED AS STATUS because a fresh install is exactly when
     * something is most likely misconfigured, and Status answers "is
     * anything wrong?" - but the user's point stands that VOLUME is
     * what this window is mostly opened FOR on an ordinary day
     * (2026-09-18). A setting means neither answer has to win.
     *
     * By sidebar index, so it survives a module being added; out of
     * range falls back to Status rather than refusing to start.
     */
    int  start_page;

    /*
     * SHOW A BOX AFTER LOAD AND UNLOAD, listing every step.
     *
     * THREE STATES, NOT TWO: -1 unset, 0 off, 1 on. The DEFAULT
     * DIFFERS BY MODE - on for a portable copy, off when installed -
     * so "absent" and "off" have to be distinguishable or a portable
     * user who turns it off gets it back on the next read.
     *
     * WHY THE DEFAULT DIFFERS, the user's reasoning 2026-09-23:
     * someone running a portable copy is finding out what the program
     * does, and a status line saying so is gone the moment anything
     * else writes there. Someone who installed it already knows, and
     * a box after every Load becomes something to dismiss.
     *
     * Use vlhe_show_apply_result() rather than reading this - it
     * resolves the unset case against the mode.
     */
    int  show_apply_result;
};

int vlhe_prefs(struct vlhe_prefs *out);
int vlhe_set_prefs(const struct vlhe_prefs *in);

/*
 * SHOULD THE APPLY RESULT BOX BE SHOWN? The setting when it has one,
 * otherwise the mode's default - on for portable, off for installed.
 */
int vlhe_show_apply_result(void);

/* What this machine's X server actually has, read rather than
 * guessed - the target ships helvetica, times, courier, lucida and
 * charter at 8/10/12/14/18/24. A list of fonts that are not installed
 * is worse than no list. */
int vlhe_font_families(char out[][VLHE_NAME_MAX], int max);
int vlhe_font_sizes(int *out, int max);

/* ------------------------------------------------------------------ */
/* Modules - what is loaded, for every module's status line           */
/* ------------------------------------------------------------------ */

struct vlhe_status {
    int  vsound_loaded;
    int  vmidi_loaded;
    int  vdisc_loaded;
    int  vsoundd_running;
    int  vdiscd_running;
    char card[VLHE_NAME_MAX];       /* the real card behind vsound      */
};

int vlhe_status(struct vlhe_status *out);

/* IS THE SOUND MODULE THERE AT ALL?
 *
 * REQUIRED BY design/09's "THE GUI MUST NOT BLOCK AN UNLOAD". The GUI
 * is the program most likely to be open when someone wants to rmmod,
 * so it must not hold the device open across one, and a user CAN
 * unload from a terminal while the window sits there. A volume view
 * showing stale sliders for a module that is gone is the failure to
 * avoid.
 *
 * So the backend reopens per operation rather than holding a handle,
 * and this says whether the last attempt found anything. The Volume
 * module greys itself out rather than lying. */
int vlhe_sound_present(void);

/* ------------------------------------------------------------------ */
/* The CARD's mixer - design/09, "save on unload, restore after load"  */
/* ------------------------------------------------------------------ */

/* NOT OUR VOLUMES. This is /dev/mixer: the hardware's own master, PCM,
 * CD, Mic and the rest, shared with KMix and everything else, and it
 * persists until something changes it.
 *
 * WHY IT IS HERE AT ALL, since design/07 leaves the card's mixer to
 * KMix: levels reset on every module load, not just every boot, which
 * on these machines is constant. The user's case - mute the mic, load
 * the modules, it is unmuted again. KMix 1.1 cannot save levels at
 * all, so nothing on the platform does this.
 *
 * OPTIONAL AND OFF BY DEFAULT (design/09). Nobody on this system has
 * ever had levels persist, so on-by-default would surprise; off means
 * today's behaviour exactly.
 *
 * The restore runs AFTER the card's driver loads, so it overrides that
 * driver's own initialisation - including the patched esssolo1's mic
 * mute. That is deliberate: it restores what the USER set. */

struct vlhe_mixer_chan {
    int  id;                        /* SOUND_MIXER_* - the OSS constant */
    char name[VLHE_NAME_MAX];       /* "Master", "Mic", "CD"            */
    int  left;                      /* 0-100                            */
    int  right;                     /* 0-100; == left when mono         */
    int  stereo;                    /* is right meaningful              */
};

/* ENUMERATE WHAT THE CARD ACTUALLY HAS, never all 25 OSS channels.
 * SOUND_MIXER_READ_DEVMASK is a bitmask of the ones this card
 * implements - the Acer's ESS Solo-1 reports ten - so a save writes
 * what exists rather than collecting EINVAL from the rest. */
int vlhe_mixer_channels(struct vlhe_mixer_chan *out, int max);

/* Is saving/restoring enabled, and set it. Persisted in the config
 * file, not here. */
int vlhe_mixer_restore_enabled(void);
int vlhe_mixer_set_restore(int on);

/* PER-PROGRAM LEVELS ACROSS A RELOAD - design/33 1c. The setting
 * ([Sound Settings] SaveProgramLevels, off by default); the file
 * (VLHE_VOLUMES, a portable folder's `volumes', or
 * /var/lib/vlhe/volumes); and the plan's two steps. save_all returns
 * how many programs it wrote (0 is a written, empty file) or -1;
 * restore_all how many it pushed, 0 when there is no file, or -1. */
/* AUTOMATIC VOICE REDUCTION'S TUNABLES - [Midi Settings] AutoVoice*,
 * config only (design/21 16). The six, in autovoice.h's order
 * (AUTOVOICE_NSET - restated here because this header does not include
 * the daemon's); -1 with the reason and the defaults if the file's set
 * is refused. `changed' says whether any differs from the defaults. */
#define VLHE_AUTOVOICE_N 6
int  vlhe_autovoice_settings(int v[VLHE_AUTOVOICE_N], int *changed,
                             const char **why);
/* THE SETTER - the Advanced Settings page, 2026-10-08. Checked by the
 * daemon's own autovoice_set(): each in range AND emergency < drain <
 * healthy, or -1 with the reason and nothing changed. Memory only, as
 * every setter; File > Save writes. */
int  vlhe_set_autovoice_settings(const int v[VLHE_AUTOVOICE_N],
                                 const char **why);
void vlhe_autovoice_spec(const int v[VLHE_AUTOVOICE_N], char *out); /* >=128 */

int vlhe_progvol_enabled(void);
int vlhe_progvol_set_enabled(int on);
const char *vlhe_progvol_state_path(void);
int vlhe_progvol_save_all(const char *path);
int vlhe_progvol_restore_all(const char *path);

/* THE `.old' BACKUP of the SYSTEM file - only that file has one
 * (design/33 section 3). Written since the beginning and never read
 * back until 2026-09-19; the first-run dialog now offers a restore
 * when one exists. `when' fills buf with a local date for the dialog
 * to show and returns 0 if unknown. restore COPIES rather than
 * renames, so the backup survives, and re-reads both files so the
 * restored settings are live immediately. */
int vlhe_conf_backup_exists(void);
int vlhe_conf_backup_when(char *buf, size_t len);
int vlhe_conf_restore_backup(struct vlhe_commit_err *err);

/* THE INTERFACE SETTINGS - the USER's file, ~/.vlhe/vlhe.conf, not
 * /etc/vlhe.conf. VerticalSliders had no setter at all until
 * 2026-09-19: the checkbox moved a static and redrew, so it applied
 * for the session and was lost at the next start. Set is MEMORY
 * ONLY; vlhe_commit() writes. */
int vlhe_ui_vertical_sliders(void);
int vlhe_ui_set_vertical_sliders(int on);

/*
 * THE CD+G VIEWER'S OPTIONS - the user's file, [CDG Viewer]. Until
 * 2026-10-01 the Options dialog stored them in the page's statics and
 * nothing wrote or read them (design/47 G5); the user: "should save as
 * well the zoom border anchor and infofields including the order of
 * them". Set is memory-only and validates; vlhe_commit_user() writes.
 *
 *   zoom          1 or 2
 *   anchor        1..9, a numpad cell
 *   track_format  0 title, 1 artist - title
 *   border_mask   0/1, hide the outer tile
 *   clear_on_eject 0/1
 *   info_fields   comma list of title performer songwriter composer
 *                 arranger message track time, IN ORDER
 */
#define VLHE_CDG_FIELDS_MAX 128
struct vlhe_cdg_prefs {
    int  zoom;
    int  anchor;
    int  track_format;
    int  border_mask;
    int  clear_on_eject;
    int  compact_detached;      /* detached: Attach and the track list on
                                 * a second row, a narrower window      */
    char info_fields[VLHE_CDG_FIELDS_MAX];
};
int vlhe_cdg_prefs(struct vlhe_cdg_prefs *out);
int vlhe_set_cdg_prefs(const struct vlhe_cdg_prefs *in);  /* -1 refused */

/*
 * WHICH DRIVE THE CD+G VIEWER SHOWS AND PLAYS - [CDG Viewer] Drive in
 * the user's file, 0..7, default 0 (/dev/vdisc0). Chosen by a radio
 * button on the CD page's drive rows, hidden when there is one drive -
 * the user, 2026-10-03 (design/31 C9). Set is memory-only and marks the
 * user half dirty; File > Save Configuration keeps it, as with every
 * other option (design/51). The viewer uses it while that drive holds
 * a disc with audio, and the first drive that does otherwise.
 */
int vlhe_cdg_drive(void);
int vlhe_set_cdg_drive(int index);                        /* -1 refused */

/*
 * AND TWO RULES FOR IT, on the CD page's Options (2026-10-03), the
 * user's file, memory-only like the rest:
 *
 *   FollowAnyDisc  1 (default): the chosen drive while it has an audio
 *                  disc, else the first drive that does. 0: the chosen
 *                  drive only - "no disc until you select a different
 *                  one".
 *   StopOnSwap     0 (default): a swap leaves the old disc playing.
 *                  1: the disc the VIEWER was playing stops.
 */
int vlhe_cdg_follow_any(void);
int vlhe_set_cdg_follow_any(int on);                      /* 0 or 1 */
int vlhe_cdg_stop_on_swap(void);
int vlhe_set_cdg_stop_on_swap(int on);                    /* 0 or 1 */

/* Write the USER half only, if dirty, never creating the file - for a
 * dialog whose OK means "keep this" on a page with no Apply. Same
 * answers as vlhe_commit(); the system half is left as it is. */
int vlhe_commit_user(struct vlhe_commit_err *err);
/* Is the USER half still unsaved? vlhe_dirty() answers for both files,
 * and a dialog that wrote only the user half must not blame a dirty
 * /etc on a missing user file. */
int vlhe_user_dirty(void);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* Called once at startup. Returns 0 always - a backend that cannot
 * reach anything is not an error, it is an empty machine. */
int vlhe_backend_init(void);
void vlhe_backend_fini(void);

/* Which implementation is linked in, for the title bar and the About
 * box. The fake one says so loudly: a screenshot taken against scripted
 * state must not be mistakable for one taken against a real machine. */
const char *vlhe_backend_name(void);

#endif /* VLHE_BACKEND_H */
