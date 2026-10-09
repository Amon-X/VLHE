/*
 * vlhe_mixer.c - see vlhe_mixer.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>

#include <linux/soundcard.h>

#include "vlhe_modconf.h"  /* a look-only open must not load a module */
#include "vlhe_mixer.h"
#include "vlhe_conf.h"
#include "vlhe_self.h"

/* OSS packs a stereo level into one int: left in the low byte, right
 * in the next, each 0-100 already. So the conversion the GUI needs is
 * masking rather than scaling - and 0-100 is what struct
 * vlhe_mixer_chan documents, so no arithmetic is lost either way. */
#define MIX_LEFT(v)   ((v) & 0xff)
#define MIX_RIGHT(v)  (((v) >> 8) & 0xff)
#define MIX_PACK(l,r) (((r) << 8) | (l))

static const char *const mixer_names[SOUND_MIXER_NRDEVICES] =
    SOUND_DEVICE_NAMES;
static const char *const mixer_labels[SOUND_MIXER_NRDEVICES] =
    SOUND_DEVICE_LABELS;

const char *
vlhe_mixer_device(void)
{
    const char *v = getenv("VLHE_MIXER");

    return (v != NULL && *v != '\0') ? v : "/dev/mixer";
}

/* THE DEVICE SEAM. Every call into the card goes through these three,
 * so a test can supply its own card without macro-rewriting the code
 * under test - which is what the first version of the test tried, and
 * it collided with these definitions.
 *
 * A seam the code offers deliberately is better than one a test has
 * to force, and it costs three function pointers that are never
 * reassigned on a real machine. */
struct vlhe_mixer_ops {
    int (*open)(const char *path, int flags);
    int (*ioctl)(int fd, unsigned long req, int *arg);
    int (*close)(int fd);
};

static int
real_open(const char *path, int flags)
{
    return open(path, flags);
}

static int
real_ioctl(int fd, unsigned long req, int *arg)
{
    return ioctl(fd, req, arg);
}

static int
real_close(int fd)
{
    return close(fd);
}

static struct vlhe_mixer_ops mixer_ops = {
    real_open, real_ioctl, real_close
};

/* THE MIXER BEING WORKED ON, when the walk below sets one - NULL
 * means vlhe_mixer_device(), the single mixer everything else uses. */
static const char *g_dev;

/* Open the mixer read-only or read-write. Returns -1 quietly when
 * there is no card - which is a machine without sound, not a fault. */
static int
mixer_open(int flags)
{
    const char *dev = g_dev != NULL ? g_dev : vlhe_mixer_device();
    struct stat ms;

    /* LOOKING MUST NOT LOAD A MODULE - design/55 R11, design/54 D53 (2026-10-04): the volume page,
     * sysinfo-vlhe's report and a mixer save all come through here, as
     * root on the target. A mixer node's minor on an empty unit with an
     * alias would load a driver on open; nothing is opened, as for a
     * machine without a card. */
    if (stat(dev, &ms) == 0 && S_ISCHR(ms.st_mode)
        && (int) major(ms.st_rdev) == 14
        && vlhe_modconf_open_would_load((int) minor(ms.st_rdev), NULL, 0)) {
        errno = ENODEV;
        return -1;
    }
    return mixer_ops.open(dev, flags);
}

/* SOUND_DEVICE_LABELS ARE PADDED to five characters ("Vol  ", "Mic  ")
 * so a 1990s text mixer could column them up. A GUI label must not
 * carry the padding, so trim it. */
static void
copy_label(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);

    while (n > 0 && src[n - 1] == ' ')
        n--;
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int
vlhe_mixer_read(struct vlhe_mixer_chan *out, int max)
{
    int fd, i, n = 0;
    int devmask = 0, stereo = 0;

    fd = mixer_open(O_RDONLY);
    if (fd < 0)
        return 0;

    if (mixer_ops.ioctl(fd, SOUND_MIXER_READ_DEVMASK, &devmask) < 0) {
        mixer_ops.close(fd);
        return 0;
    }
    /* Not fatal: a card that cannot say which channels are stereo is
     * reported as all-mono, which shows one slider instead of two. */
    if (mixer_ops.ioctl(fd, SOUND_MIXER_READ_STEREODEVS, &stereo) < 0)
        stereo = 0;

    for (i = 0; i < SOUND_MIXER_NRDEVICES && n < max; i++) {
        int v;

        if (!(devmask & (1 << i)))
            continue;
        if (mixer_ops.ioctl(fd, MIXER_READ(i), &v) < 0)
            continue;           /* claimed but unreadable - skip it */

        out[n].id     = i;
        out[n].left   = MIX_LEFT(v);
        out[n].right  = MIX_RIGHT(v);
        out[n].stereo = (stereo & (1 << i)) ? 1 : 0;
        if (!out[n].stereo)
            out[n].right = out[n].left;
        copy_label(out[n].name, mixer_labels[i], sizeof out[n].name);
        n++;
    }

    mixer_ops.close(fd);
    return n;
}

int
vlhe_mixer_write(int id, int left, int right)
{
    int fd, v, rc;

    if (id < 0 || id >= SOUND_MIXER_NRDEVICES)
        return -1;
    if (left < 0)   left = 0;
    if (left > 100) left = 100;
    if (right < 0)   right = 0;
    if (right > 100) right = 100;

    fd = mixer_open(O_WRONLY);
    if (fd < 0) {
        /* Some drivers refuse O_WRONLY on a mixer; O_RDWR always
         * works where writing is possible at all. */
        fd = mixer_open(O_RDWR);
        if (fd < 0)
            return -1;
    }

    v = MIX_PACK(left, right);
    rc = mixer_ops.ioctl(fd, MIXER_WRITE(id), &v) < 0 ? -1 : 0;
    mixer_ops.close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Save and restore                                                   */
/* ------------------------------------------------------------------ */

int
vlhe_mixer_save(struct vlhe_conf *c, const char *section)
{
    struct vlhe_mixer_chan ch[SOUND_MIXER_NRDEVICES];
    int n, i;

    n = vlhe_mixer_read(ch, SOUND_MIXER_NRDEVICES);
    for (i = 0; i < n; i++) {
        char val[32];

        /* "left,right" even when mono - one shape to parse, and a
         * mono channel reads back with both the same. */
        sprintf(val, "%d,%d", ch[i].left, ch[i].right);
        if (vlhe_conf_set(c, section, mixer_names[ch[i].id], val) < 0)
            return i;           /* table full: what we managed */
    }
    return n;
}

int
vlhe_mixer_restore(const struct vlhe_conf *c, const char *section)
{
    struct vlhe_mixer_chan ch[SOUND_MIXER_NRDEVICES];
    int n, i, done = 0;

    /* READ THE CARD FIRST so we only write channels it HAS. A config
     * carried from another machine then degrades - the channels both
     * cards share are restored, the rest ignored - rather than
     * failing or writing something meaningless. */
    n = vlhe_mixer_read(ch, SOUND_MIXER_NRDEVICES);

    for (i = 0; i < n; i++) {
        const char *v = vlhe_conf_get(c, section, mixer_names[ch[i].id],
                                      NULL);
        int left, right;
        char *end;

        if (v == NULL || *v == '\0')
            continue;           /* not in the file - leave it alone */

        /* CHECKED BEFORE IT IS WRITTEN - design/47 B7. strtol() on a
         * value that is not a number gives 0, and 0,0 is a muted
         * channel; a hand-edited or damaged line used to silence that
         * channel on every start. A value that is not two numbers in
         * 0..100 with nothing after them is skipped, like an absent
         * one. */
        left = (int)strtol(v, &end, 10);
        if (end == v)
            continue;
        if (*end == ',') {
            const char *rs = end + 1;

            right = (int)strtol(rs, &end, 10);
            if (end == rs)
                continue;
        } else {
            right = left;
        }
        while (*end == ' ' || *end == '\t')
            end++;
        if (*end != '\0' || left < 0 || left > 100 || right < 0 || right > 100)
            continue;

        if (vlhe_mixer_write(ch[i].id, left, right) == 0)
            done++;
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* Every mixer on the machine - saved at unload, restored at load     */
/* ------------------------------------------------------------------ */

/*
 * SAVE MIXER LEVELS, BUILT - 2026-10-02. design/36 row 70: the save and
 * restore above were written and unit-tested and NOTHING CALLED THEM,
 * so the setting greyed widgets and did nothing else. The user's design
 * (2026-09-25): "walk through all the mixers and save those. So on boot
 * it would restore the users mixers"; and 2026-10-02, "save at shutdown
 * restore on boot", restoring AFTER the card's driver has loaded,
 * because loading a driver sets its mixer to the driver's defaults.
 *
 * WHEN: the unload plan's (mixersave) and the load plan's
 * (mixerrestore), in vlhe_apply.c, when SaveMixerLevels is set. So the
 * init script's `stop' (K40, at shutdown) saves and its `start' (S60,
 * after S20modutils has loaded the card from /etc/modules) restores -
 * and the control centre's Unload and Load do the same.
 *
 * EVERY MIXER, NOT /dev/mixer. vsound registers no mixer, so each one
 * is a real card's; a machine may have several (86Box: es1371 and sb).
 *
 * KEYED BY THE CARD, NOT THE NUMBER. Mixer numbers SHUFFLE when drivers
 * reload (register_sound_mixer() takes the first free minor - row 70,
 * defect 3), so a level saved as "mixer 1" could land on another card.
 * SOUND_MIXER_INFO names the card - "ES1371", "Solo1", and for the
 * legacy drivers soundcard.c's get_mixer_info() returns their own id
 * ("SB" ...). Two cards with one name are told apart by order; a driver
 * that answers nothing (trident, i810_audio, vwsnd in 2.2) falls back
 * to the mixer's number.
 *
 * A FILE OF ITS OWN, not /etc/vlhe.conf: it is rewritten at every
 * unload, and the config is the user's to edit. Sections are
 * [Mixer <card>], keys the OSS short names vlhe_mixer_save() writes.
 */

#define MIX_MAX_NODES 8

const char *
vlhe_mixer_state_path(void)
{
    static char buf[VLHE_PATH_MAX];
    const char *env = getenv("VLHE_MIXERS");

    if (env != NULL && *env != '\0') {
        strncpy(buf, env, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        return buf;
    }
    /* A PORTABLE FOLDER KEEPS IT BESIDE ITSELF, as it does the session
     * file - a copy that promises to touch nothing of the system. */
    if (vlhe_self_is_trial()
        && vlhe_self_path("mixers", buf, sizeof buf))
        return buf;
    strcpy(buf, "/var/lib/vlhe/mixers");
    return buf;
}

struct mix_node {
    char path[32];
    int  minor;
    char section[64];
};

/* The card's own name, letters and digits only, or "" when the driver
 * does not answer SOUND_MIXER_INFO. */
static void
card_id(const char *path, char *out, size_t max)
{
    mixer_info info;
    int fd, i, n = 0;

    out[0] = '\0';
    fd = mixer_ops.open(path, O_RDONLY);
    if (fd < 0)
        return;
    memset(&info, 0, sizeof info);
    if (mixer_ops.ioctl(fd, SOUND_MIXER_INFO, (int *) &info) == 0) {
        info.id[sizeof info.id - 1] = '\0';    /* strncpy'd: may lack it */
        for (i = 0; info.id[i] != '\0' && n + 1 < (int) max; i++) {
            char c = info.id[i];

            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9'))
                out[n++] = c;
        }
        out[n] = '\0';
    }
    mixer_ops.close(fd);
}

/*
 * EVERY MIXER THAT ANSWERS, in minor order, each with its section name.
 * The candidates are the usual names; one device reachable by two names
 * (/dev/mixer and /dev/mixer0 are the same 14,0) is taken once, and a
 * node whose minor has no driver (open fails, ENXIO) is skipped.
 */
static int
mixer_walk(struct mix_node *out, int max)
{
    static const char *const cand[] = {
        "/dev/mixer", "/dev/mixer0", "/dev/mixer1", "/dev/mixer2",
        "/dev/mixer3", "/dev/mixer4", "/dev/mixer5", "/dev/mixer6",
        "/dev/mixer7"
    };
    int n = 0, i, j;
    const char *only = getenv("VLHE_MIXER");

    /* THE TEST OVERRIDE, as vlhe_mixer_device() honours it: one named
     * device and nothing else, without the device-node checks - so a
     * host test can walk a fake card through the seam above. */
    if (only != NULL && *only != '\0' && max > 0) {
        strncpy(out[0].path, only, sizeof out[0].path - 1);
        out[0].path[sizeof out[0].path - 1] = '\0';
        out[0].minor = 0;
        n = 1;
    }

    for (i = 0; n == 0 && i < (int) (sizeof cand / sizeof cand[0]) && n < max;
         i++) {
        struct stat sb;
        int fd, mn, dup = 0;

        if (stat(cand[i], &sb) != 0 || !S_ISCHR(sb.st_mode)
            || (int) major(sb.st_rdev) != 14)
            continue;
        mn = (int) minor(sb.st_rdev);
        if ((mn & 15) != 0)
            continue;                   /* not a mixer minor */
        for (j = 0; j < n; j++)
            if (out[j].minor == mn)
                dup = 1;
        if (dup)
            continue;
        /* LOOKING MUST NOT LOAD A MODULE - CLAUDE.md section 5: an
         * empty minor makes soundcore ask for an alias, and an unloaded
         * soundcore makes the kernel ask for char-major-14. Skipped
         * when that could load something; vlhe_modconf.h has the one
         * case this can hide (a minor held by a driver other than the
         * alias's). */
        if (vlhe_modconf_open_would_load(mn, NULL, 0))
            continue;
        fd = mixer_ops.open(cand[i], O_RDONLY);
        if (fd < 0)
            continue;                   /* no driver behind it */
        mixer_ops.close(fd);

        strncpy(out[n].path, cand[i], sizeof out[n].path - 1);
        out[n].path[sizeof out[n].path - 1] = '\0';
        out[n].minor = mn;
        n++;
    }

    /* MINOR ORDER, so "the second ES1371" means the same card each
     * time the walk runs on an unchanged machine. */
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && out[j - 1].minor > out[j].minor; j--) {
            struct mix_node t = out[j];

            out[j] = out[j - 1];
            out[j - 1] = t;
        }

    for (i = 0; i < n; i++) {
        char id[20];
        int  same = 0;

        card_id(out[i].path, id, sizeof id);
        if (id[0] == '\0')
            sprintf(id, "mixer%d", out[i].minor / 16);
        for (j = 0; j < i; j++) {
            char other[20];

            card_id(out[j].path, other, sizeof other);
            if (other[0] == '\0')
                sprintf(other, "mixer%d", out[j].minor / 16);
            if (strcmp(other, id) == 0)
                same++;
        }
        if (same == 0)
            sprintf(out[i].section, "Mixer %s", id);
        else
            sprintf(out[i].section, "Mixer %s %d", id, same + 1);
    }
    return n;
}

int
vlhe_mixer_save_all(const char *path)
{
    static struct vlhe_conf c;          /* large: not on the stack */
    struct mix_node node[MIX_MAX_NODES];
    int n, i, cards = 0;

    if (path == NULL)
        path = vlhe_mixer_state_path();
    memset(&c, 0, sizeof c);
    n = mixer_walk(node, MIX_MAX_NODES);
    for (i = 0; i < n; i++) {
        g_dev = node[i].path;
        if (vlhe_mixer_save(&c, node[i].section) > 0)
            cards++;
        g_dev = NULL;
    }
    if (cards == 0)
        return 0;                       /* no card: nothing to keep */
    (void) vlhe_conf_mkdir_for(path);
    if (vlhe_conf_write(&c, path,
            "The sound cards' mixer levels, saved by VLHE at unload and\n"
            "restored at load when Save Mixer Levels is set. One section\n"
            "per card, named by the card. Rewritten at every unload.",
            0) != 0)
        return -1;
    return cards;
}

int
vlhe_mixer_restore_all(const char *path)
{
    static struct vlhe_conf c;
    struct mix_node node[MIX_MAX_NODES];
    struct stat sb;
    int n, i, cards = 0;

    if (path == NULL)
        path = vlhe_mixer_state_path();
    if (stat(path, &sb) != 0)
        return 0;                       /* never saved: nothing to do */
    memset(&c, 0, sizeof c);
    if (vlhe_conf_read(&c, path) < 0)
        return -1;
    n = mixer_walk(node, MIX_MAX_NODES);
    for (i = 0; i < n; i++) {
        g_dev = node[i].path;
        if (vlhe_mixer_restore(&c, node[i].section) > 0)
            cards++;
        g_dev = NULL;
    }
    return cards;
}
