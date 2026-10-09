/*
 * vlhe_mod_sound.h - the Sound Settings module's interface.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Same shape as the other modules. Both its pages are forms - the card
 * and -R reach the config the pump starts from, the MIDI slot is a
 * module parameter - so both want the commit trio.
 */

#ifndef VLHE_MOD_SOUND_H
#define VLHE_MOD_SOUND_H

#include <gtk/gtk.h>

GtkWidget *sound_build(void (*report_fn)(const char *),
                       void (*page_fn)(int));

int  sound_collect(void);       /* read the widgets into the backend */
void sound_reload(void);        /* put them back - Cancel's half     */
int  sound_dirty(void);         /* bitmask of tabs with unsaved edits */
void sound_machine(void);       /* modules changed - repaint what says so */
void sound_privilege_changed(void); /* Modify pressed - regrey, no reload */
void sound_set_dirty_cb(void (*cb)(void));
int  sound_page_wants_buttons(void);
void sound_set_page(int page);

#endif /* VLHE_MOD_SOUND_H */
