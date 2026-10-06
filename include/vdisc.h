/*
 * vdisc.h - the vdisc/vdiscd interface.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * The virtual CD-ROM half of the rewrite. design/07-vsound.md section 1
 * for the four components; section 3.1 for how CD audio reaches the
 * mixer now.
 *
 * WHAT CHANGED FROM vcd.h, AND WHY EACH THING CHANGED.
 *
 * The old protocol was not simply renamed. Section 7's rule is that
 * nothing is ported without answering three questions - does it still
 * have a job, do its assumptions still hold, was it right in the first
 * place - and the opcode list is where that bit hardest.
 *
 * KEPT, because the job and the assumptions are unchanged:
 *
 *   READ        the block path. This is the product and it works.
 *   PLAY        start CDDA at an LBA, ending at another
 *   PAUSE       )
 *   RESUME      ) the transport controls the CD-ROM API demands
 *   STOP        )
 *   SUBCHNL     where playback has reached, for CDROMSUBCHNL
 *
 * DROPPED:
 *
 *   VOLREAD     defined in vcd.h and implemented NOWHERE. Dead on
 *               arrival; not carried forward as an OPCODE - the kernel
 *               answers CDROMVOLREAD from its cached value instead.
 *
 * RESTORED 2026-08-26, having been wrongly dropped:
 *
 *   VOLCTRL     dropped on the reasoning that `vcdd' implemented it
 *               badly - it called set_pcm_volume(), opening the CARD's
 *               mixer and setting SOUND_MIXER_PCM, a GLOBAL control
 *               attenuating everything the card plays. That criticism
 *               was right and the conclusion was wrong: the DEFECT was
 *               touching the card's mixer, not the ioctl existing.
 *
 *               CDROMVOLCTRL means "set the volume of the CD CHANNEL",
 *               and vdiscd holds exactly one - so there IS something to
 *               do, and vsound already has the mechanism.
 *
 *               Made a no-op instead, it was silently discarded: a user
 *               moved KsCD's volume slider 128 times in one run with no
 *               effect. Returning 0 rather than -ENOSYS made it silent
 *               as well as useless.


 * C89, and no long long in anything crossing the boundary.
 */

#ifndef _VDISC_H
#define _VDISC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <sys/types.h>
/*
 * The daemon compiles against libc, which has no <linux/types.h>.
 *
 * __u8 belongs in here as much as __u32 does: the TOC structs use it
 * for track numbers and flags, and leaving it out broke only the
 * USERSPACE build - the kernel side gets it from linux/types.h and
 * compiled clean, so the omission was invisible until the host build
 * ran. That asymmetry is the reason both builds are kept.
 */
#ifndef __u32
#define __u32 unsigned int
#define __s32 int
#define __u8  unsigned char
#endif
#endif

/*
 * HOW MANY DRIVES, AND WHY IT IS NOT THE SAME QUESTION AS HOW MANY
 * VCHANS.
 *
 * A vchan is private to vsound.o: nothing outside indexes chan[], a
 * client arrives through open() and the module picks a slot, so
 * growing that array is an internal decision.
 *
 * A drive is not private. Three arrays are handed to the BLOCK LAYER
 * as bare int *:
 *
 *     blk_size[major] = ...; blksize_size[major] = ...;
 *     hardsect_size[major] = ...;
 *
 * and ll_rw_blk.c:646 does
 *
 *     blk_size[major][MINOR(bh->b_rdev)]
 *
 * with NO bound check. The kernel reads whatever minor the device NODE
 * names, so `mknod /dev/vdisc9 b 250 9' plus a dd reaches index 9
 * whatever the driver thinks it registered. The array must therefore
 * cover every minor that can exist, and it cannot be reallocated under
 * a live block layer - 2.2 has no RCU and no way to make that pointer
 * swap safe.
 *
 * So the CEILING is fixed at load and the array is always allocated at
 * full size; what is dynamic is which slots hold a disc.
 *
 * cdemu (virtualcd_0.3/virtualcd.c) reached the same conclusion
 * independently, and it is a driver that shipped doing this job on
 * this kernel generation:
 *
 *     static int max_virtualcds = 8;                          :123
 *     MODULE_PARM(max_virtualcds, "i");                       :727
 *     if (max_virtualcds < 1 || max_virtualcds > 256) ...      :742
 *     virtualcd_sizes = kmalloc(max_virtualcds * ...);         :766
 *     blk_size[MAJOR_NR] = virtualcd_sizes;                    :794
 *
 * kmalloc'd once in init_module, kfree'd in cleanup, every bound check
 * against max_virtualcds (:205, :395, :642, :684), and nothing resizes.
 * Its 256 is the minor space itself - the hardware limit, not a chosen
 * number.
 *
 * WE DIFFER FROM cdemu IN TWO WAYS, both deliberate:
 *
 *   - the ceiling is 8, not 256. A user asked for eight and there is
 *     no use case here for more; a smaller fixed ceiling means the
 *     array can just be static, with no kmalloc to fail and no free
 *     path to get wrong on the error branch. Eight ints is 32 bytes.
 *
 *   - the DEFAULT is 1, not 8. One disc is the normal case. The drives
 *     beyond the first cost a device node each and clutter what the
 *     desktop shows, so a user who wants two says so.
 *
 * DYNAMIC MEANS WHAT IT SOUNDS LIKE: a disc attaches into a free slot
 * at runtime and detaches from it, with no rmmod and no rebuild.
 * `discs=' sets how many drives are ADVERTISED at load; the array
 * behind them is always VDISC_MAX_DEVS wide, so raising the count is
 * the only thing that needs a reload, and it needs one only because
 * the CD-ROM layer registers each drive by name at init.
 */
#define VDISC_MAX_DEVS      8       /* /dev/vdisc0 .. 7, the ceiling  */
#define VDISC_DEF_DEVS      1       /* advertised unless discs= says  */
#define VDISC_MAX_TRACKS    99      /* the CD standard's own limit    */
#define VDISC_MAX_XFER      (64 * 1024)

/*
 * Geometry. Fixed by the CD standard, so there was nothing to
 * reconsider when these crossed over - 2048 and 2352 are what the discs
 * are, and 512 is the unit the block layer counts in whatever we think.
 */
/*
 * PROTOCOL 2, 2026-09-15. Version 1 carried one sector form per track
 * and a boolean pre_emphasis; 2 adds VDISC_FORM_MODE2, turns that
 * boolean into the Q channel's control nibble, and gives the spare
 * byte beside it a meaning (the session). The layout is
 * unchanged; the MEANING of bytes is, so the number moves and a stale
 * bundle - module from one build, daemon from another, which has
 * happened on the P3 - refuses at attach instead of misreading.
 */
#define VDISC_PROTO_VERSION 2
#define VDISC_SECTOR_SIZE   2048    /* CD-ROM logical sector (cooked)   */

/*
 * A raw CD frame: 2352 bytes, which is what CDDA is and what every
 * backend's read_audio() returns. CD_FRAMESIZE_RAW in the kernel's own
 * header; spelled out here because vdisc.h is shared with userspace,
 * where that header is not included.
 */
#define VDISC_RAW_FRAME     2352

/*
 * The most raw frames one READ_AUDIO may ask for.
 *
 * THE UNIFORM LAYER'S OWN CAP IS 8 (cdrom.c:1916, "do max 8 frames at
 * the time") and 86Box run 100 confirmed it on a target: every
 * GPCMD_READ_CD arrived as either 18816 bytes (8 frames) or 7056 (3),
 * always paired, a 10-frame request split as 8 + 3. So 8 is what
 * actually arrives - this is the bound, not a guess, and 8 * 2352 =
 * 18816 sits well inside VDISC_MAX_XFER.
 */
#define VDISC_MAX_RAW_FRAMES  8

/*
 * The most Mode 2 sectors one READ_MODE2 may ask for.
 *
 * The uniform layer asks for ONE at a time - cdrom.c:1877 passes
 * nblocks=1 for CDROMREADMODE2, unlike the audio path's 8 - so this
 * bound is generous rather than tight. 8 * 2336 = 18688, which still
 * sits inside VDISC_MAX_XFER, and matching the audio cap means one
 * number to remember instead of two.
 */
#define VDISC_MAX_MODE2_SECTORS  8
#define VDISC_RAW_SECTOR    2352    /* raw frame as stored in BIN/IMG   */

/*
 * A MODE 2 sector as CDROMREADMODE2 wants it: the raw frame from its
 * SUBHEADER onward, i.e. without the 12-byte sync and 4-byte header.
 * CD_FRAMESIZE_RAW0 in the kernel's own header (2352 - 12 - 4);
 * spelled out here because vdisc.h is shared with userspace, where
 * that header is not included.
 *
 * It is ALSO the stride of a 2336-byte MODE2/2336 file, which is the
 * same sector already stored this way - so one constant serves both.
 */
#define VDISC_MODE2_SECTOR  2336

/* A CD+G frame: 2352 plus 96 bytes of interleaved subchannel. */
#define VDISC_CDG_SECTOR    2448

/*
 * THE SUBCHANNEL ITSELF: 96 bytes, eight channels P..W of 12 bytes
 * each, interleaved. VDISC_CDG_SECTOR is exactly VDISC_RAW_SECTOR
 * plus this, and both CD+G and CD-TEXT ride the R and W channels -
 * see vsound/cdtext.h for the 96-to-72 unpack they share.
 */
#define VDISC_SUB_SIZE      96
#define VDISC_HARDSECT      512     /* unit the block layer counts in   */
#define VDISC_MODE1_DATA_OFF 16     /* user data inside a raw MODE1     */
/*
 * CD-ROM XA (Mode 2): the 8-byte subheader follows the header at 16,
 * duplicated at 20, and the user data starts at 24. Byte 18 is the
 * submode; bit 5 set means FORM 2 - 2324 bytes of user data and no
 * ECC, which a 2048-byte block read cannot return. The form is a
 * property of EACH SECTOR, not of the track: the Video CD and CD-i
 * discs in ExampleCDs/ mix both within track 1 (design/27 3c), so the
 * daemon decides from the sector's own subheader and the kernel is
 * told only that the track is Mode 2.
 */
#define VDISC_XA_SUBHDR_OFF  16     /* first copy of the subheader      */
#define VDISC_XA_SUBHDR_SIZE 4      /* bytes per copy                   */
#define VDISC_XA_SUBMODE_OFF 18     /* the submode byte                 */
#define VDISC_XA_FORM2       0x20   /* submode bit: Form 2              */

/*
 * The pieces of a raw sector, by size. A 2352-byte frame is:
 *
 *   0    12   sync        00 FF FF FF FF FF FF FF FF FF FF 00
 *   12    4   header      MSF (BCD) + mode byte
 *   16    8   subheader   Mode 2 only: two identical 4-byte copies
 *   16/24     user data   2048 (Mode 1, Mode 2 Form 1) or 2324 (Form 2)
 *   ...  288  EDC/ECC     Mode 1 and Mode 2 Form 1 only
 *
 * Form 2 spends its EDC/ECC space on user data instead: 2324 + 4
 * bytes of optional EDC, which is why 2048 + 288 and 2324 + 12 both
 * come to 2336 from the subheader onward.
 */
#define VDISC_SYNC_SIZE      12
#define VDISC_HDR_SIZE       4
#define VDISC_MODE_OFF       15     /* the mode byte, last of the header */
#define VDISC_FORM2_DATA     2324   /* user data in a Mode 2 Form 2      */
/*
 * THE EDC/ECC TAIL IS NOT ONE SIZE, and assuming it was cost a wrong
 * length on the first build of vdisc_sector_piece_len():
 *
 *   MODE 1         EDC 4 + PAD 8 + ECC 276 = 288
 *   MODE 2 FORM 1  EDC 4 +         ECC 276 = 280
 *
 * Both sectors are 2352 bytes. Mode 2 Form 1 spends 8 of them on the
 * sub-header, and takes them back from the pad - which is why MMC-2
 * 6.1.13 says of the EDC/ECC bit "For Mode 1 CD format, this will
 * include the 8 bytes of pad data". That sentence IS this difference.
 *
 * Mode 2 Form 2 has neither: its 2324-byte user data covers the space,
 * with 4 optional EDC bytes the standard does not require.
 */
#define VDISC_EDC_MODE1      288    /* EDC + pad + ECC                   */
#define VDISC_EDC_FORM1      280    /* EDC + ECC, no pad                 */
/*
 * FORM 2 HAS A TAIL TOO, and missing it was the second bug in the
 * first build of read_pieces: 4 bytes at 2348, after 2324 of user
 * data. The standard calls it optional and a disc may leave it zero -
 * the Sega Video CD's sectors carry a real value - but the BYTES ARE
 * THERE either way, and a caller asking for the EDC/ECC field of a
 * Form 2 sector should get them rather than nothing.
 *
 * 12 + 4 + 8 + 2324 + 4 = 2352, which is how the arithmetic closes.
 */
#define VDISC_EDC_FORM2      4      /* EDC only, and optional at that    */
#define VDISC_C2_SIZE        294    /* C2 pointers: one BIT per byte     */
#define VDISC_MODE2_DATA_OFF 24     /* user data inside a raw MODE2     */

/*
 * Red Book timing. The KERNEL side uses <linux/cdrom.h>'s own CD_FRAMES
 * and CD_MSF_OFFSET (cdrom.h:328-330) rather than these, so the two
 * cannot drift where it matters; these exist because userspace has no
 * such header and msf.c needs the same constants.
 *
 * The +150: THE DRIVER ADDS IT AND THE UNIFORM LAYER STRIPS IT. Backend
 * LBAs stay file-relative. Getting this backwards shifts every address
 * by two seconds in both formats and looks plausible until diffed.
 */
#define VDISC_FRAMES_PER_SEC 75
#define VDISC_MSF_OFFSET    150

/*
 * A ceiling on any single disc, well above a real one (450000 sectors
 * is ~880 minutes) and well below where sectors * 2048 overflows an
 * int. Both halves matter: attach validates against this so a daemon
 * cannot send a value that goes negative on assignment and makes the
 * size computation wrap.
 */
#define VDISC_MAX_SECTORS   450000

/*
 * The most sectors one block request may span. VDISC_MAX_XFER already
 * bounds the BYTES; this bounds the SECTOR COUNT independently, because
 * the two are computed from different fields and a request that passes
 * one can still be absurd on the other.
 */
#define VDISC_MAX_SECTORS_PER_REQ  (VDISC_MAX_XFER / VDISC_SECTOR_SIZE + 1)

/* The Q channel's control bits, as carried in vdisc_track_info.control
 * and reported in cdte_ctrl / cdsc_ctrl. CDROM_DATA_TRACK (0x04) is
 * NOT stored there - is_data is. */
#define VDISC_CTRL_PRE_EMPHASIS 0x01
#define VDISC_CTRL_COPY_PERMIT  0x02
#define VDISC_CTRL_DATA         0x04
#define VDISC_CTRL_FOUR_CHANNEL 0x08

/*
 * Sector forms a backend may present - ONE PER TRACK on the wire. The
 * kernel uses the value for cdte_datamode alone (1 for Mode 1, 2 for
 * Mode 2); which bytes of a raw frame are user data is the daemon's
 * business, and for MODE2 it is decided sector by sector.
 */
#define VDISC_FORM_ISO_2048  0  /* cooked; read straight through        */
#define VDISC_FORM_MODE1_2352 1 /* raw; data at VDISC_MODE1_DATA_OFF    */
#define VDISC_FORM_AUDIO_2352 2 /* raw CDDA                             */
#define VDISC_FORM_MODE2      3 /* raw XA; form per sector, see above   */

/* ------------------------------------------------------------------ *
 * TOC, handed to the kernel at attach
 *
 * Cached in the module because audio_ioctl can be called with locks
 * held - a round trip to userspace at ioctl time could deadlock. The
 * daemon sends the whole table once, so there is never anything to ask
 * for later. This was right in vcd.h and is unchanged.
 * ------------------------------------------------------------------ */

struct vdisc_track_info {
    __u32   start_lba;      /* first sector of the track, disc LBA      */
    __u32   length;         /* sectors                                  */
    __u8    is_data;        /* 1 = data track, 0 = audio                */
    /*
     * THE Q CHANNEL'S CONTROL NIBBLE - protocol 2. Was a boolean
     * pre_emphasis, which reported two of the four bits and always
     * said 0 for the other two (design/27 4b).
     *
     *   0x01 pre-emphasis        0x02 digital copy permitted
     *   0x04 data track          0x08 four-channel audio
     *
     * The input carries all four: a CCD has Control= per entry, a CUE
     * has FLAGS PRE / DCP / 4CH. The DATA bit is left out of here and
     * kept in is_data, which the module and the whole block path
     * already key on; READTOCENTRY and SUBCHNL OR it back in. So a
     * protocol-1 daemon's 0 or 1 in this byte still reads correctly.
     *
     * Neither missing bit is severe - nothing on this platform
     * enforces SCMS, and quadraphonic CDs barely exist - but a player
     * showing disc properties showed wrong ones, and the cost is a
     * mask.
     */
    __u8    control;        /* 0x01 PRE | 0x02 DCP | 0x08 4CH           */
    __u8    sector_form;    /* VDISC_FORM_*                             */
    /*
     * WHICH SESSION the track is in, 1-based - protocol 2. A CloneCD
     * set says so on every entry (Session=); a CUE says REM SESSION n.
     * The kernel needs it for ONE thing: CDROMMULTISESSION, which
     * isofs asks before it reads a volume descriptor, must answer
     * with the first track of the LAST session and xa_flag set when
     * there is more than one - that is how ide-cd defines it
     * (ide-cd.c:1745, first_session != last_session) and isofs uses
     * the address ONLY when the flag is set (fs/isofs/inode.c:464).
     * A daemon that sends 0 everywhere is a single-session disc.
     */
    __u8    session;
};

/*
 * LBAs here are FILE-RELATIVE and stay that way. The driver adds the
 * +150 MSF offset and the uniform layer strips it; getting that
 * backwards shifts every address by two seconds in both formats and
 * looks plausible until diffed.
 */
struct vdisc_attach {
    __u32   proto_version;
    __u32   minor;
    __u32   total_sectors;  /* == lead-out LBA                          */
    __u32   data_sectors;   /* mountable, VDISC_SECTOR_SIZE units       */
    /*
     * data_start IS ALWAYS 0 SINCE PROTOCOL 2 (2026-09-15), and the
     * field is kept so the layout does not move and the kernel's
     * bounds check on it still runs against an untrusted daemon.
     *
     * WHAT IT WAS. From 2026-08-25 the block device was a WINDOW ONTO
     * THE DATA TRACK: block LBA 0 meant data_start, so a disc whose
     * data track began after audio (the Blue Book / Enhanced CD
     * shape) could be mounted by adding the offset to every block
     * request. It was a stand-in for multi-session support.
     *
     * WHY IT HAD TO GO. On a real drive block LBA equals disc LBA, and
     * isofs finds a later session's filesystem by asking
     * CDROMMULTISESSION for the session's start and reading the volume
     * descriptor at that ABSOLUTE address plus 16 (fs/isofs/inode.c:
     * 549), then following extents that are absolute disc addresses
     * too (:691). With the window AND a correct CDROMMULTISESSION the
     * same offset is applied twice: bluebook's data track at 6750
     * would have isofs ask for block 6766 and the window read file
     * sector 13516, past the lead-out. The two cannot coexist, and the
     * ioctl is the one a drive has. The window was never caught
     * because the fixture behind it was filler, not a filesystem.
     *
     * What the window allowed that a drive does not: mounting a
     * SINGLE-session disc whose data track follows audio. A drive
     * cannot mount that either, so nothing real is lost.
     *
     * So: the block device covers the disc from 0 to the end of the
     * last data track (data_sectors), a block read that lands in an
     * audio track is refused by the daemon as a drive refuses it, and
     * a second session is reached the way isofs reaches it on
     * hardware - through CDROMMULTISESSION and the session byte in
     * each track.
     */
    __u32   data_start;     /* ALWAYS 0 - see above                     */
    __u32   leadout_lba;
    __u8    first_track;
    __u8    last_track;
    __u8    n_tracks;
    __u8    pad;
    struct vdisc_track_info track[VDISC_MAX_TRACKS];
};

/* ------------------------------------------------------------------ *
 * The TOC, cached in the module at attach.
 * ------------------------------------------------------------------ *
 *
 * CACHED IN THE KERNEL DELIBERATELY, and this is the one piece of the
 * old design that must not be simplified away: audio_ioctl() can be
 * called with locks held, so it must never round-trip to userspace.
 * CLAUDE.md section 5 records this as a constraint that cost time to
 * find.
 */
/* ------------------------------------------------------------------ *
 * Request / reply
 * ------------------------------------------------------------------ */

#define VDISC_OP_READ       1       /* read data sectors              */
#define VDISC_OP_PLAY       2       /* start CDDA playback            */
#define VDISC_OP_PAUSE      3
#define VDISC_OP_RESUME     4
#define VDISC_OP_STOP       5
#define VDISC_OP_SUBCHNL    6       /* where playback has reached     */
#define VDISC_OP_VOLCTRL    7       /* set the CD channel's volume    */
/*
 * STALL: the kernel to the daemon, not the other way. Sent once when
 * the drive was PLAYing and the daemon's audio child stopped reporting
 * its position for VDISC_REPORT_STALE (vdisc_mod.c) - Acer run 85,
 * 2026-09-15: the child sat in D state on a pulled CF card, the drive
 * said PLAY with a frozen position for two minutes, and the parent
 * learned of it only when told to shut down. lba carries the position
 * the drive was left at. Fire-and-forget, like the other audio ops.
 */
#define VDISC_OP_STALL      8       /* the audio child stopped reporting */
/*
 * READ_AUDIO: raw 2352-byte CDDA frames, for CDROMREADAUDIO through
 * generic_packet. design/27 section 8.
 *
 * WHY IT IS NOT VDISC_OP_READ WITH A FLAG. That one is defined in
 * VDISC_SECTOR_SIZE (2048) units all the way down - serve_read()
 * multiplies by it, the kernel's bounds check divides by it, and the
 * block layer's own geometry is in those units. A raw frame is 2352
 * bytes and belongs to a different address space: audio frames are
 * addressed by disc LBA with no data window, and a count that is legal
 * in one unit overflows the transfer in the other. Two opcodes cost
 * ten lines and keep the unit unambiguous at every layer.
 *
 * lba is the disc LBA of the first frame; count is frames, NOT
 * sectors. The reply payload is count * VDISC_RAW_FRAME bytes.
 *
 * SYNCHRONOUS, unlike the audio ops above - the caller sleeps for the
 * reply, which is safe because the uniform layer reaches
 * generic_packet from cdrom_ioctl having just done a GFP_KERNEL
 * allocation itself (cdrom.c:1917).
 */
#define VDISC_OP_READ_AUDIO 9       /* raw 2352-byte CDDA frames      */
/*
 * READ_MODE2: 2336-byte sectors from the subheader onward, for
 * CDROMREADMODE2 through generic_packet. design/27 section 3a.
 *
 * A THIRD OPCODE FOR THE SAME REASON THERE IS A SECOND: the unit is
 * different again (2336, against READ's 2048 and READ_AUDIO's 2352),
 * and a count legal in one overflows the transfer in another. Three
 * opcodes cost ten lines each and keep the unit unambiguous at every
 * layer.
 *
 * lba is the disc LBA of the first sector; count is sectors. The reply
 * payload is count * VDISC_MODE2_SECTOR bytes.
 *
 * SYNCHRONOUS, like READ_AUDIO, and it shares that path's machinery -
 * see vdisc_raw_read() in vdisc_mod.c.
 *
 * THE FORM IS NOT CONSULTED. CDROMREADMODE2 asks for a fixed 2336
 * whatever the sector holds, so both Form 1 and Form 2 come back
 * bytes-as-stored and the caller decides from the subheader it now
 * has. That is the point: read_data() REFUSES Form 2, as a drive
 * refuses a READ(10) of one, and Form 2 is exactly what a Video CD's
 * MPEG track is.
 */
#define VDISC_OP_READ_MODE2 10      /* 2336-byte Mode 2 sectors       */
/*
 * READ_RAW: 2352-byte sectors from ANY track, for CDROMREADRAW.
 *
 * SAME BYTES AS READ_AUDIO, DIFFERENT MEANING, AND THE DIFFERENCE IS
 * A REFUSAL. READ_AUDIO refuses a DATA track, because asking to play
 * one as audio is an error - MMC-2 Table 3 makes it ILLEGAL REQUEST,
 * and vcd_mod.c's version of that bug streamed filesystem bytes to
 * the card as samples.
 *
 * `CDROMREADRAW' IS NOT A PLAY REQUEST. It asks for raw sectors from
 * wherever they are, and a real drive returns them. **On a Video CD
 * EVERY TRACK IS DATA**, so routing it through READ_AUDIO refuses the
 * whole disc - which is exactly what happened on 86Box run 107: 75
 * raw reads served by the kernel, 75 refused by the daemon, and
 * MPlayer saw ENOSYS for all of them.
 *
 * So the unit is shared and the POLICY is not. lba is the disc LBA,
 * count is sectors, payload is count * VDISC_RAW_SECTOR.
 */
#define VDISC_OP_READ_RAW   11      /* 2352-byte sectors, any track   */
/*
 * READ_CD: the general form - the caller names WHICH PIECES of each
 * sector it wants, and gets them concatenated. design/30.
 *
 * WHY THIS REPLACES THE THREE ABOVE. MMC-2 Table 88's byte 9 is FIVE
 * INDEPENDENT FIELDS - sync, header, sub-header, user data, EDC/ECC,
 * plus a C2 selector - and both references decode it bit by bit:
 * cdemu assembles the reply field by field, 86Box does the same with
 * one function per sector type. **We matched the whole byte against
 * two constants**, which is right for the four requests
 * `cdrom_ioctl` makes and wrong for anything else - and
 * `CDROM_SEND_PACKET` hands an application's own command block
 * straight through, a path we opened by advertising the capability.
 *
 * The user's objection, 2026-09-17, and the record agrees with it:
 * "nothing on this platform is known to use it - THAT WE KNOW OF".
 * CD-TEXT, an MP3 encoder and cdparanoia's reachability were all
 * absence claims that turned out false.
 *
 * `arg0' carries VDISC_SEC_* below. READ_AUDIO, READ_RAW and
 * READ_MODE2 remain as three particular masks, so nothing that works
 * today changes shape.
 */
#define VDISC_OP_READ_CD    12      /* pieces named by arg0           */

/*
 * EJECT - the client asked for the medium, and the daemon owns it.
 *
 * FIRE-AND-FORGET, like STOP and VOLCTRL. The uniform layer calls
 * tray_move() in contexts where a round trip is not available, so the
 * driver refuses or accepts on state it already holds (`locked') and
 * asks the daemon to drop the image afterwards.
 *
 * WHY THE DRIVER DOES NOT JUST SET A FLAG - cdemu is the reference
 * (vcd/contrib/refs/cdemu, command_start_stop_unit,
 * device-commands.c:2645): an eject UNLOADS THE DISC, so the TOC
 * stops reading and the medium is genuinely gone. Ours used to set
 * tray_open and return 0 with the image still attached, and a CD
 * player that ejected then read eleven tracks off the drive it had
 * just emptied - measured on 86Box 2026-09-19.
 */
#define VDISC_OP_EJECT      13      /* unload the image, cdemu-style  */

/*
 * READ_SUB: `count' sectors of RAW SUBCHANNEL from `lba', 96 bytes
 * each, P through W exactly as the disc carries them.
 *
 * NO CALLER TODAY - 2026-10-05. vdiscd serves it (serve_sub()) and
 * nothing issues it: the module never sends this op, so no program can
 * read subchannel through a drive, and the CD+G Viewer reads the image
 * file itself with vdisc_image_read_sub() (design/34 section 6e2,
 * decided 2026-09-26, six days after this op was added). KEPT, by the
 * user's call, for a module passthrough (a program asking a drive for
 * subchannel through generic_packet) or a Q-channel CDROMSUBCHNL
 * (design/27 section 4a). Nothing exercises it, so its first user
 * should test it.
 *
 * design/34 has the scope. The short version: CD+G and CD-TEXT both
 * ride the R-W channels, the decoders for both are built and tested
 * (lib/cdg.c, lib/cdtext.c), and when this was written nothing could
 * ask for the bytes - image.h:155 said a 2448-byte frame "yields its
 * first 2352 (the subchannel is ignored here)". This op was to be how.
 *
 * WHY NOT A VDISC_SEC_* BIT. All eight are taken, and the subchannel
 * is not part of the 2352-byte frame in the first place - it is a
 * parallel channel recorded alongside, which is why a rip that keeps
 * it needs 2448 bytes per sector rather than a larger frame.
 *
 * RAW, NOT UNPACKED, AND NOT INTERPRETED. The daemon hands over what
 * is on the disc:
 *
 *   - the unpack lives in userspace where it is tested, and doing it
 *     twice is two things to keep in step;
 *   - the Q channel is wanted in its own right - design/27 section 4a
 *     records it as the authoritative source for the control nibble
 *     and for CDROMSUBCHNL, both of which we approximate today;
 *   - THE LAYOUT IS NOT KNOWABLE HERE. Our two karaoke discs disagree
 *     (one cooked RW96, one interleaved P-W) and a CUE does not record
 *     which - cdemu hardcodes a guess at exactly this point and
 *     carries a FIXME saying so. The caller sniffs; see cdg.h.
 *
 * -ENODATA when the track's file has no room for subchannel, which is
 * a different answer from 96 zero bytes: QUAKE106.sub is a COMPLETE
 * capture whose R-W is all zeros, so "recorded and empty" and "never
 * recorded" are both real and distinguishable.
 */
#define VDISC_OP_READ_SUB   14      /* 96 bytes of raw P-W per sector */

/*
 * The pieces of a raw 2352-byte sector, in the order they occur - and
 * therefore the order they are returned, which is what the standard
 * requires.
 *
 *   sync        12   0..11     00 FF x10 00
 *   header       4   12..15    MSF + mode byte
 *   subheader    8   16..23    Mode 2 only; two identical 4-byte copies
 *   data      2048   16 or 24  where it starts depends on the form
 *   edc/ecc    288   varies    Mode 1 and Mode 2 Form 1 only
 *
 * VDISC_SEC_AUDIO is not a piece but a POLICY bit: it says the caller
 * asked for CD-DA specifically (expected sector type 1), so a DATA
 * track must be refused. MMC-2 Table 3 makes that ILLEGAL REQUEST,
 * and 86Box run 107 showed what applying it unasked costs - it
 * refused an entire Video CD.
 */
#define VDISC_SEC_SYNC      0x01    /* 12 bytes at offset 0           */
#define VDISC_SEC_HEADER    0x02    /* 4 bytes at offset 12           */
#define VDISC_SEC_SUBHDR    0x04    /* 8 bytes at offset 16           */
#define VDISC_SEC_DATA      0x08    /* the user data, form-dependent  */
#define VDISC_SEC_EDC       0x10    /* the EDC/ECC tail               */
#define VDISC_SEC_C2        0x20    /* 294 bytes of C2, ZERO-FILLED   */
#define VDISC_SEC_C2BLOCK   0x40    /* 296: C2 plus block error byte  */
#define VDISC_SEC_AUDIO     0x80    /* POLICY: refuse a data track    */


/*
 * Audio status, mirrored from the kernel's <linux/cdrom.h>:361-366.
 *
 * These are THE API's OWN VALUES, not ours - a client comparing against
 * CDROM_AUDIO_PLAY expects 0x11, and vdisc_do_put_pos() validates
 * against the CDROM_AUDIO_* names directly. The duplication exists
 * because userspace has no <linux/cdrom.h>; the kernel side of this
 * module never uses these spellings.
 *
 * If they ever disagree with the kernel header, the kernel header is
 * right.
 */
#define VDISC_AUDIO_INVALID     0x00
#define VDISC_AUDIO_PLAY        0x11
#define VDISC_AUDIO_PAUSED      0x12
#define VDISC_AUDIO_COMPLETED   0x13
#define VDISC_AUDIO_ERROR       0x14
#define VDISC_AUDIO_NO_STATUS   0x15

/* ------------------------------------------------------------------ *
 * Wire messages
 * ------------------------------------------------------------------ */

/*
 * A request handed to the daemon. The handle is an opaque cookie
 * echoed in the reply - the kernel finds the pending request BY HANDLE
 * and never by a pointer from userspace, so a confused or hostile
 * daemon cannot make the kernel dereference an address of its choosing.
 */
struct vdisc_req {
    __u32   handle;
    __u32   opcode;         /* VDISC_OP_*                               */
    __u32   minor;
    __u32   lba;            /* READ: start sector; PLAY: start sector   */
    __u32   count;          /* READ: sector count                       */
    __u32   arg0;           /* PLAY: end lba (exclusive)                */
    __u32   arg1;           /* PLAY: track number                       */
    __u32   reserved;
};

/*
 * A reply. NARROWER THAN vcd_reply, which carried arg0/arg1/arg2 for
 * SUBCHNL results - the kernel used to ASK the daemon where playback
 * had reached. That is gone: SUBCHNL is polled every frame by some
 * games and must never round-trip to userspace, so the position is
 * PUSHED down through struct vdisc_pos instead and those three fields
 * have no remaining use.
 */
struct vdisc_reply {
    __u32   handle;
    __s32   status;         /* 0 = ok, else -errno                      */
    __u32   length;         /* bytes of payload following (READ)        */
    __u32   reserved;
};

/*
 * Where playback has reached, pushed by the daemon as it feeds the
 * channel. Also carries status, so the daemon can report a track
 * ending or its CDDA child dying without being asked.
 *
 * This exists because the position CANNOT be computed in the kernel any
 * more. vcd_mod.c derived it from elapsed jiffies at 75 frames/second,
 * which held while the daemon wrote /dev/dsp directly and the OSS
 * buffer paced it. The audio now goes into a vchan and is mixed, so
 * what the card has actually reached is something vsound knows and a
 * clock in the kernel does not.
 */
struct vdisc_pos {
    __u32   minor;
    __u32   lba;            /* absolute, file-relative                  */
    __u32   status;         /* CDROM_AUDIO_*                            */
    __u32   track;
};

/* ------------------------------------------------------------------ *
 * Control ioctls, on /dev/vdiscctl
 *
 * The magic is 'D', not vcd.h's 'V' - vsound.h already uses 'V', and
 * the two modules are loaded at the same time on the same system.
 * Sharing a magic would not break anything by itself, but it makes a
 * mismatched ioctl land somewhere plausible instead of failing.
 *
 * SND_GET/SND_DONE/SND_TUNE are NOT here. They were vcdsnd's channel to
 * its daemon; sound has its own module and its own device now.
 * ------------------------------------------------------------------ */

#define VDISC_IOC_MAGIC     'D'
#define VDISC_IOC_ATTACH    _IOW(VDISC_IOC_MAGIC, 1, struct vdisc_attach)
#define VDISC_IOC_DETACH    _IOW(VDISC_IOC_MAGIC, 2, int)

/*
 * DETACH ANYWAY - the minor OR'd with this means "the CD-ROM layer
 * has already decided", and the use_count check is skipped.
 *
 * WHY IT EXISTS. A plain DETACH refuses while anything holds the
 * drive (use_count > 0), which is right for the GUI's Eject button
 * and for shutdown: both mean "nobody at all". It is WRONG for a
 * client eject, and that is measured rather than argued -
 * cdrom_ioctl's CDROMEJECT refuses unless use_count == 1
 * (cdrom.c:1466) and only then calls tray_move(), so the one
 * remaining holder IS the program asking to eject. Refusing it
 * there is refusing the caller on its own behalf.
 *
 * WHAT A REAL DRIVE DOES, from a strace of grip on 86Box's own
 * ATAPI drive with nothing of ours loaded
 * (tests/logs/2026-09-20-crash-realdrive): CDROMEJECT returns 0
 * while grip still holds the fd, and every ioctl after it returns
 * -EIO. The medium goes; the descriptor stays valid and useless.
 * A MOUNTED FILESYSTEM DOES NOT STOP IT EITHER - the user ejected
 * a mounted disc with a file open in GIMP, and
 * tests/logs/2026-09-20-86box-atapi-eject-crash shows ISO 9660
 * mounting and then a clean eject on the same drive. Refusing a
 * mounted eject is `eject(1)' being careful (it reads mtab,
 * eject.c:418), not the drive.
 *
 * SO THE ONLY GATE IS THE LOCK, which is checked in tray_move()
 * before the request is ever sent.
 */
#define VDISC_DETACH_FORCE  0x40000000

/*
 * READ THE DRIVE'S OWN LOCK BACK - our one device-specific ioctl,
 * on /dev/vdiscN rather than the control device.
 *
 * WHY IT HAS TO EXIST. 2.2 keeps `keeplocked' as ONE FILE-SCOPE
 * STATIC for every CD-ROM on the system (cdrom.c:260) and offers no
 * ioctl to read it. So the GUI kept its own locked_state[] array and
 * drew the checkbox from that - a per-drive cache of a global that
 * ANY program's CDROM_LOCKDOOR can change. Lock a drive, let
 * anything unlock any drive, and the box still shows locked while
 * the kernel is not. The user hit that twice.
 *
 * The driver now holds the real per-drive flag; this is how to ask
 * for it. Third argument is an `int *'.
 *
 * WHY dev_ioctl AND NOT THE CONTROL DEVICE. /dev/vdiscctl admits one
 * opener and that is vdiscd (vdisc.h's header), so the GUI cannot
 * use it. cdrom_ioctl falls through to cdo->dev_ioctl for anything
 * it does not recognise (cdrom.c:1797), gated on CDC_IOCTLS - the
 * layer's documented escape hatch for exactly this. Being a
 * FALL-THROUGH, it cannot shadow a standard ioctl: every CDROM* code
 * is handled above and never reaches us.
 */
#define VDISC_IOC_GET_LOCK  _IOR(VDISC_IOC_MAGIC, 6, int)
#define VDISC_IOC_GET_REQ   _IOR(VDISC_IOC_MAGIC, 3, struct vdisc_req)
#define VDISC_IOC_PUT_REPLY _IOW(VDISC_IOC_MAGIC, 4, struct vdisc_reply)
#define VDISC_IOC_PUT_POS   _IOW(VDISC_IOC_MAGIC, 5, struct vdisc_pos)

#endif /* _VDISC_H */
