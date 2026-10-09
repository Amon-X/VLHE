/*
 * vlhe_mixer.h - the CARD's mixer, /dev/mixer.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * NOT vsound's per-client volumes. This is the hardware's own Master,
 * PCM, CD, Mic and the rest - shared with KMix and everything else on
 * the machine, and persisting until something changes it.
 *
 * WHY WE TOUCH IT AT ALL, since design/07 leaves the card's mixer to
 * KMix: levels reset on every MODULE LOAD, not just every boot, which
 * on these machines is constant. The user's case is muting the mic,
 * loading the modules, and finding it unmuted. KMix 1.1 cannot save
 * levels at all, so nothing on this platform does it.
 *
 * Separated from the rest of the backend because it is one device node
 * and a handful of ioctls, and because a machine with no card at all
 * must still open the GUI - every call here fails softly.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_MIXER_H
#define VLHE_MIXER_H

#include "vlhe_backend.h"       /* struct vlhe_mixer_chan */

/* Read every channel the card actually implements.
 *
 * ENUMERATED FROM THE DEVICE, never all 25 OSS channels:
 * SOUND_MIXER_READ_DEVMASK is a bitmask of the ones this card has -
 * the Acer's ESS Solo-1 reports ten - so a save writes what exists
 * rather than collecting EINVAL from the other fifteen.
 *
 * Returns how many were written; 0 when there is no mixer, which is a
 * machine with no sound card and not an error. */
int vlhe_mixer_read(struct vlhe_mixer_chan *out, int max);

/* Set one channel. `right' is ignored on a mono channel. Returns 0,
 * or -1 if the device or that channel could not be written. */
int vlhe_mixer_write(int id, int left, int right);

/* Save every channel to, and restore from, a config file section.
 * Values are stored under their OSS SHORT NAME ("vol", "mic", "cd" -
 * SOUND_DEVICE_NAMES) rather than by index, so a file written on one
 * card is meaningful on another and a human can read it.
 *
 * Restore SKIPS a channel the current card does not have, so carrying
 * a config between machines degrades rather than failing.
 *
 * Both return the number of channels handled, or -1 on a file error. */
struct vlhe_conf;               /* opaque here - see vlhe_conf.h */
int vlhe_mixer_save(struct vlhe_conf *c, const char *section);
int vlhe_mixer_restore(const struct vlhe_conf *c, const char *section);

/* The device this module talks to. Overridable for testing, as the
 * config paths are - a shipped machine uses /dev/mixer. */
const char *vlhe_mixer_device(void);

/*
 * EVERY CARD'S MIXER, to and from a file of its own - SAVE MIXER LEVELS
 * (2026-10-02; vlhe_mixer.c has the account). Walks /dev/mixer and
 * /dev/mixer0-7, names each by its card (SOUND_MIXER_INFO), and saves
 * or restores it in [Mixer <card>]. `path' NULL means
 * vlhe_mixer_state_path().
 *
 * save returns the cards saved (0 for none - no file is written), or
 * -1 if the file could not be written. restore returns the cards
 * restored (0 when there is no file - never saved), or -1 if it could
 * not be read.
 */
int vlhe_mixer_save_all(const char *path);
int vlhe_mixer_restore_all(const char *path);

/* /var/lib/vlhe/mixers, or `mixers' in a portable folder, or VLHE_MIXERS. */
const char *vlhe_mixer_state_path(void);

#endif /* VLHE_MIXER_H */
