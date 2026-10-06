/*
 * vlhe_mod_render.h - Render MIDI: a .mid in, a .wav or .mp3 out.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHY IT IS A GUI PAGE AND NOT A DAEMON SETTING. Nothing here touches
 * a module, a device or a running daemon: it picks two filenames and
 * runs `smf2wav', which is an offline renderer that opens no audio
 * device at all. So it works on a machine where vsound is not
 * loaded, where the card is busy, or where there is no card - which
 * is most of the reasons someone would want a file instead of sound.
 *
 * A MOD_VIEW, therefore, with no collect/reload/dirty trio: there is
 * nothing to commit. The output name and the LAME path are typed and
 * used immediately, not saved into /etc/vlhe.conf, because they
 * belong to one render rather than to the machine.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_MOD_RENDER_H
#define VLHE_MOD_RENDER_H

#include <gtk/gtk.h>

GtkWidget *render_build(void (*report_fn)(const char *));

/*
 * THE ADVANCED TAB IS A FORM, so the page takes the settings trio
 * after all - the user asked for OK/Apply/Cancel on it, 2026-09-22,
 * and for the values to persist.
 *
 * THE RENDER TAB IS NOT. Its filename boxes belong to one render and
 * are deliberately not saved; only the Advanced tab's values reach
 * [Render Settings] in the user config.
 */
int  render_collect(void);
void render_reload(void);
int  render_dirty(void);
void render_machine(void);   /* fonts or modules changed - refill lists */

/*
 * TELL THE SHELL WHEN A CONTROL CHANGES, so the sidebar's asterisk
 * appears. Every other settings module takes this and Render did
 * not - the flag was set correctly and nothing repainted, so the
 * page looked clean while holding unsaved edits. Found by the user,
 * 2026-09-22.
 */
void render_set_dirty_cb(void (*cb)(void));
/* ASKED BEFORE EVERY RENDER; a 0 cancels it. The shell uses it to
 * offer saving unsaved Midi/Render settings first (2026-09-24). */
void render_set_before_cb(int (*cb)(void));

/*
 * WHERE OK GOES. The shell's OK normally leaves for the user's start
 * page; this page asks to stay and show its FIRST tab instead - the
 * user, 2026-09-22: "This okay should return the Render tab instead
 * of the settings is that possible?"
 *
 * It is, and it is the right shape: someone who has just set up a
 * comparison render wants to run it, and the button that means
 * "done with these settings" should put them in front of the thing
 * the settings were for.
 */
void render_show_first_tab(void);

/*
 * THE WINDOW IS CLOSING - design/54 D58 (design/36 row 116), 2026-10-04.
 * A render in progress is cancelled exactly as Cancel cancels it: the
 * loop stops smf2wav or LAME (SIGTERM, then SIGKILL after two seconds,
 * on the pid it forked) and removes the partial file. Closing used to
 * leave the render running to the end, minutes on a slow machine, with
 * no window to show it. Returns 1 if a render was running.
 */
int  render_abort(void);

#endif /* VLHE_MOD_RENDER_H */
