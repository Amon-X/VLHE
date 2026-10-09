/*
 * vlhe_mod_midi.h - the Midi Settings module's interface to the shell.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Same shape as the other two modules: build it, ask which page wants
 * the button row, give it a way to report.
 */

#ifndef VLHE_MOD_MIDI_H
#define VLHE_MOD_MIDI_H

#include <gtk/gtk.h>

GtkWidget *midi_build(void (*report_fn)(const char *),
                      void (*page_fn)(int));

/* Sound Fonts and Options are forms - they write the config and the
 * daemon rereads. Driver is a form too, and needs root besides. So
 * unlike Volume and CD, every page here wants the commit trio. */
int  midi_collect(void);        /* read the widgets into the backend */
void midi_reload(void);       /* put them back - Cancel's half     */
int  midi_dirty(void);        /* unsaved edits on this page        */
void midi_machine(void);      /* modules changed - repaint what says so */
void midi_privilege_changed(void); /* Modify pressed - regrey, no reload */
/* The shell's unsaved-settings question before "Restart Synth" - the
 * same callback the Status page's Restart buttons use (design/47 M6). */
void midi_set_before_restart_cb(int (*cb)(const char *daemon));
void midi_set_dirty_cb(void (*cb)(void));
int  midi_page_wants_buttons(void);

void midi_set_page(int page);

/* RE-APPLY WHAT THE PAGE HID. gtk_widget_show_all() is recursive and
 * re-shows the SoundFont frame's other half, so the shell calls this
 * after it - the same contract volume_sync_visibility() has. */
void midi_sync_visibility(void);

#endif /* VLHE_MOD_MIDI_H */
