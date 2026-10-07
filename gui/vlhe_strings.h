/*
 * vlhe_strings.h - every word the GUI shows, in one place, and a
 *                  tooltip slot beside every one that can carry it.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * STATUS: WIRED AND FULL - corrected 2026-10-04 (design/54 T26). The
 * wiring pass this paragraph describes was done module by module, and
 * every module includes this file; the body holds the GUI's strings.
 * What follows is the original status, kept as the record:
 *
 * THE FRAME ONLY, 2026-10-02. NOT WIRED, AND THE BODY IS
 * EMPTY ON PURPOSE. This replaces vlhe_str.h (2026-09-30), whose
 * body was written ahead of 135 commits of GUI change and was wrong
 * in enough places that auditing it would have cost more than
 * writing it again (design/47 section 7; the user, 2026-10-02,
 * "create a new strings file. keep what you suggested. Do not start
 * on it"). The preamble below is that file's, carried over because
 * the design in it was the hard part and has not aged. The body is
 * written by the wiring pass, FROM THE CALL SITES, module by module
 * in screen order, one commit per module - after the test round of
 * design/36 rows 104-119 has come back and before release.
 *
 * ================= HOW TO CHANGE A LABEL =================
 *
 * Edit the text here and rebuild. Nothing else. Do not edit the
 * literal at the call site - there should not be one.
 *
 * ================= EVERY ITEM HAS A TOOLTIP SLOT =================
 *
 * THE USER'S RULE, 2026-10-02: "I would like every item to have a
 * tooltip option even if it is currently blank so that I can easily
 * add one unless an item can not have a tooltip."
 *
 * So every string that names a WIDGET comes as a pair:
 *
 *     #define STR_RND_BTN_RENDER      _("Render")
 *     #define STR_RND_BTN_RENDER_TIP  _("")
 *
 * and adding a tooltip is filling in the second line. BLANK MEANS
 * NONE: the wiring helper sets no tooltip for an empty string, so a
 * blank slot costs nothing at run time and never shows an empty
 * yellow box. Translators see the pair together.
 *
 * WHAT CAN TAKE ONE, in GTK 1.2 - a tooltip needs a widget with its
 * own X window, and gtk_tooltips_set_tip() on anything else is
 * silently useless:
 *
 *   | gets a _TIP slot            | how                               |
 *   |-----------------------------|-----------------------------------|
 *   | buttons, check and radio    | directly                          |
 *   | entries, spin buttons       | directly                          |
 *   | option menus                | directly (the button part)        |
 *   | sliders, lists, the sidebar | directly                          |
 *   | LABELS, including a         | the slot exists; the wiring pass  |
 *   | field's caption and a       | wraps the label in a GtkEventBox  |
 *   | frame's title               | ONLY where the slot is non-blank, |
 *   |                             | since a label has no window       |
 *
 *   | NO slot - cannot carry one  | why                               |
 *   |-----------------------------|-----------------------------------|
 *   | menu items (File, View...)  | 1.2 never shows a tip on one      |
 *   | notebook tab labels         | UNVERIFIED in 1.2 - the tab's     |
 *   |                             | label is ours and could be an     |
 *   |                             | event box, but whether the        |
 *   |                             | notebook delivers enter events to |
 *   |                             | it is untried; the wiring pass    |
 *   |                             | tries ONE on the fake GUI and     |
 *   |                             | moves this row up if it shows     |
 *   | the status line, say(),     | messages, not widgets             |
 *   | dialog bodies, FMT_ strings |                                   |
 *   | sidebar notice frames       | text that changes, not a control  |
 *
 * A string with no `_TIP' twin is therefore a statement that it
 * cannot have one, not an omission - so the wiring pass adds the
 * twin to EVERY widget string, blank, and leaves it off only for the
 * second table. A reader who wants a tooltip on something and finds
 * no slot has found something that cannot take one.
 *
 * THE HELPER the wiring pass writes, so a blank is really nothing:
 *
 *     void vlhe_tip(GtkWidget *w, const char *tip);
 *       - returns at once for NULL or "" (no tooltip is set)
 *       - one GtkTooltips for the whole program, made on first use
 *       - a GtkLabel is reparented into a GtkEventBox first; every
 *         other widget is given the tip directly
 *
 * ================= AND HOW IT BECOMES gettext =================
 *
 * EVERY STRING IS ALREADY WRAPPED IN `_()', which today expands to
 * nothing. Switching the whole program to gettext is ONE LINE - the
 * definition below becomes
 *
 *     #define _(s) gettext(s)
 *
 * and the call sites do not change, because they name constants
 * rather than literals. What remains after that line is `setlocale'
 * and `bindtextdomain' in main(), an `xgettext' rule, and shipping
 * the `.mo' files - none of which touches C code.
 *
 * gettext IS AVAILABLE ON THE TARGET, checked rather than assumed:
 * glibc 2.1.3 exports `gettext', `dgettext', `dcgettext' and their
 * `__'-prefixed forms (`nm -D lib/libc.so.6'), and
 * `usr/include/libintl.h' is present. So no library would need
 * shipping. THE FIRST CHECK FOR THIS MISSED THEM - the symbols carry
 * an `@@GLIBC_2.0' suffix and a grep anchored on `$' finds nothing,
 * which is CLAUDE.md section 2's false-absence trap in a new place.
 *
 * WHETHER TO TAKE THAT STEP IS UNDECIDED. gettext's value is that
 * other people can translate without recompiling; the audience for
 * a translated Corel Linux 1.2 driver may not exist. This file is
 * useful either way and forecloses nothing.
 *
 * ================= WHAT DOES NOT BELONG HERE =================
 *
 * ONLY WHAT A USER READS ON SCREEN. Not config keys, not `/proc' or
 * `/dev' paths, not module names, not trace output. Wrapping one of
 * those does not fail the build - it silently changes a string the
 * program compares against, and the failure surfaces somewhere else
 * entirely.
 *
 * The distinction is finer than it sounds. `"/dev/cdrom"' is a PATH
 * and must never come from here; `"/dev/cdrom -> %.100s"' is a
 * MESSAGE that happens to contain one, and belongs here.
 *
 * AND NO STRING THE PROGRAM TESTS AGAINST. design/47 section 7
 * fault 3: on_modify_ok() decided whether to offer a retry with
 * strstr(why, "not the root password") against wording made in
 * vlhe_priv.c. A message that is also a code path cannot be moved
 * here or reworded; it wants a return code, and that change comes
 * BEFORE the wiring pass, not during it.
 *
 * ================= THE TUI HAS ITS OWN FILE =================
 *
 * `setup-vlhe' is sh for bash 2.01.1 and cannot include this. Its
 * words live in `setup-vlhe-strings', sourced shell with the same
 * naming and a `_HELP' twin per item where this file has `_TIP' - a
 * console menu's one-line description under the menu, blank meaning
 * none. TWO FILES AND A CHECK, the user's decision 2026-10-02: the
 * forty-odd strings the two share (setting names, the law / filter /
 * rate choices, the font wording) are listed by a script in
 * `check-all' that fails when the two texts for one name differ, so
 * drift is caught mechanically. design/31 B1a has the whole shape.
 *
 * ================= FORMAT STRINGS =================
 *
 * The `FMT_' constants are `sprintf' formats, and they are here for
 * the same reason as the rest: a translator needs them, and leaving
 * them inline would mean hunting them down later.
 *
 * IF THIS EVER BECOMES gettext, THEY NEED POSITIONAL SPECIFIERS -
 * `%1$d' rather than `%d' - because word order differs between
 * languages and a translator must be able to move the argument.
 * glibc 2.1.3 supports them. They are written plainly for now
 * because positional forms are harder to read and nothing needs
 * them yet.
 *
 * ================= NAMING =================
 *
 *   STR_<MODULE>_<KIND>_<NAME>        the text
 *   STR_<MODULE>_<KIND>_<NAME>_TIP    its tooltip, or _("")
 *   FMT_<MODULE>_<NAME>               a format
 *
 * MODULE is the sidebar's: SHELL, STATUS, VOL, SND, MID, CD, CDG,
 * RND. KIND is what it is on screen: LABEL, FRAME, BTN, CHECK,
 * TOGGLE, RADIO, MENU, TITLE (a window's), MSG (the status line),
 * TEXT (a label set later, a dialog body, a result box line). TIP is
 * never a KIND - it is the suffix. STR_MOD_* are the sidebar's
 * entries, N_() because they sit in a static table.
 *
 * THE FIRST PASS NAMED BY WORDING, NOT PURPOSE - the first four
 * words of the text, upper-cased, a _2 where two texts collide - so
 * that 300 sites could be lifted mechanically and each module's
 * commit shows the screen unchanged. A name is renamed to its
 * purpose when someone touches it (filling its _TIP is the usual
 * occasion); rewording a text does not require renaming it.
 *
 * ================= ORDER =================
 *
 * Arranged the way the program looks: the shell, then the sidebar's
 * modules in their screen order, and within each its tabs in theirs,
 * then that module's dialogs. Finding a string is "where do I see
 * it" rather than "which file was that in". The section headings
 * below are that order, empty until the wiring pass fills each one
 * from its module's source.
 *
 * C89, ASCII only: this is built by GCC 2.95.2 like everything else.
 */

#ifndef VLHE_STRINGS_H
#define VLHE_STRINGS_H

/*
 * THE MARKER. Expands to nothing today; becomes `gettext(s)' to turn
 * the program over to real translation.
 */
#define _(s) (s)

/*
 * AND `N_()' FOR STRINGS IN STATIC TABLES. A table initialiser cannot
 * call a function, so the string is MARKED here and TRANSLATED where
 * it is used. `vlhe_cc.c's module table is the case that needs it.
 *
 * Identical to `_()' while `_()' does nothing. They diverge the
 * moment gettext is switched on, which is why they are separate now
 * rather than later.
 */
#define N_(s) (s)

/* ==================================================================
 * THE SHELL - the window, the menus, the sidebar, the buttons on
 * every page, the status line. vlhe_cc.c.
 * ================================================================== */

#define STR_SHELL_REVIEW_MAJOR                       N_("Block major")
#define STR_SHELL_REVIEW_DRIVES                      N_("Drives")
#define STR_SHELL_REVIEW_PACKET                      N_("Packet commands")
#define STR_SHELL_REVIEW_LINKCDROM                   N_("Point /dev/cdrom at the virtual drive")
#define STR_SHELL_REVIEW_CARDDEVICE                  N_("Card device")
#define STR_SHELL_REVIEW_PROGRAMSUSE                 N_("Programs use")
#define STR_SHELL_REVIEW_RELEASEONIDLE               N_("Release the card when idle")
#define STR_SHELL_REVIEW_MIDICHANNEL                 N_("MIDI channel")
#define STR_SHELL_REVIEW_SAVEMIXERLEVELS             N_("Save mixer levels")
#define STR_SHELL_REVIEW_VOICES                      N_("Voices")
#define STR_SHELL_REVIEW_GAIN                        N_("Gain (tenths of a percent)")
#define STR_SHELL_REVIEW_EFFECTS                     N_("Effects")
#define STR_SHELL_REVIEW_LAW                         N_("Volume curve")
#define STR_SHELL_REVIEW_FILTER                      N_("Velocity filter")
#define STR_SHELL_REVIEW_RATE                        N_("Rate")
#define STR_SHELL_REVIEW_MINOR                       N_("MIDI minor")
#define STR_SHELL_REVIEW_LOADATBOOT                  N_("Include in load")
#define STR_SHELL_FILTER_CONF                        N_("Configuration files (*.conf)")
#define STR_SHELL_FILTER_ALL                         N_("All files (*)")
#define STR_SHELL_TEXT_LEAVING_DISCARDS_DOT          _(".\n\nLeaving now discards them.")
#define STR_SHELL_TEXT_LEAVING_DISCARDS              _("\n\nLeaving now discards them.")
#define STR_SHELL_MSG_COULD_NOT_BE_RESTORED          _("could not be restored")
#define STR_SHELL_MSG_UNKNOWN_ERROR                  _("unknown error")
#define STR_SHELL_MSG_NOTHING_COULD_BE_WRITTEN       _("nothing could be written")
#define STR_SHELL_MSG_CONFIGURATION_PLACEHOLDER      _("(configuration)")
#define STR_SHELL_MSG_AUTHENTICATION_FAILED          _("authentication failed")
#define STR_SHELL_MSG_COULD_NOT_WRITE                _("could not write")
#define STR_SHELL_TEXT_THESE_SETTINGS_WOULD_CHANGE   _("These settings would change:")
#define STR_SHELL_TEXT_SAVE_TO_MAKE_THEM             _("Save to make them this machine's configuration")
#define STR_SHELL_TEXT_SAVING_THEM_NEEDS_ROOT        _("saving them needs root")
#define STR_SHELL_TITLE_LOAD_CONFIGURATION           _("Load Configuration")
#define STR_SHELL_TITLE_SAVE_DRAFT                   _("Save Draft")
#define STR_SHELL_MSG_CONFIGURATION_SAVED            _("Configuration saved")
#define STR_SHELL_MSG_CONFIGURATION_FILE_CREATED     _("Configuration file created")
#define STR_SHELL_TEXT_RESET_YOUR_OWN                _("your own settings")
#define STR_SHELL_TEXT_RESET_SYSTEM                  _("the system settings")
#define STR_SHELL_TEXT_RESET_EVERY_USER              _("every user\'s settings")
#define STR_SHELL_TEXT_RESET_ALL                     _("all settings, system and user")
#define STR_SHELL_TEXT_DEFAULTS_FROM_SEED            _("The defaults come from this machine\'s own\n/etc/vlhe/defaults.conf.")
#define STR_SHELL_TEXT_DEFAULTS_SHIPPED              _("The defaults are the ones the program ships with.")


#define STR_SHELL_APP_TITLE                          _("VLHE - Virtual Legacy Hardware Emulation")
#define STR_MOD_STATUS                               N_("Status")
#define STR_MOD_STATUS_DESC                          N_("What is loaded and running")
#define STR_MOD_VOLUME                               N_("Volume")
#define STR_MOD_VOLUME_DESC                          N_("Volume mixer for VLHE virtual audio channels")
#define STR_MOD_SOUND                                N_("Sound Settings")
#define STR_MOD_SOUND_DESC                           N_("VLHE virtual sound device configuration")
#define STR_MOD_MIDI                                 N_("Midi Settings")
#define STR_MOD_MIDI_DESC                            N_("VLHE virtual MIDI device configuration")
#define STR_MOD_CD                                   N_("CD Settings")
#define STR_MOD_CD_DESC                              N_("VLHE virtual CD-ROM and audio configuration")
#define STR_MOD_CDG                                  N_("CD+G Viewer")
#define STR_MOD_CDG_DESC                             N_("CD+G graphics viewer")
#define STR_MOD_RENDER                               N_("Render MIDI")
#define STR_MOD_RENDER_DESC                          N_("Render a MIDI file to WAV or MP3")
#define STR_SHELL_TEXT_MIDI_SETTINGS_PAGE            _("MIDI Settings")


#define STR_SHELL_LABEL_RUNNING_WITHOUT_CONFIGURATION_FILE _("Running without\na configuration\nfile.\n\nSettings apply\nnow but will not\npersist.\n\nFile / Save\nConfiguration\nwrites one.")
#define STR_SHELL_LABEL_RUNNING_WITHOUT_CONFIGURATION_FILE_TIP _("")
#define STR_SHELL_LABEL_CHANGING_THESE_SETTINGS_REQUIRES _("Changing these settings requires the\nsuperuser password.")
#define STR_SHELL_LABEL_CHANGING_THESE_SETTINGS_REQUIRES_TIP _("")
#define STR_SHELL_LABEL_PASSWORD                     _("Password:")
#define STR_SHELL_LABEL_PASSWORD_TIP                 _("")
#define STR_SHELL_LABEL_PUT_SETTINGS_BACK_DEFAULTS   _("Put settings back to the defaults. The current file is\nkept as <name>.old, so this can be undone.")
#define STR_SHELL_LABEL_PUT_SETTINGS_BACK_DEFAULTS_TIP _("")
#define STR_SHELL_LABEL_RESET                        _("Reset:")
#define STR_SHELL_LABEL_RESET_TIP                    _("")
#define STR_SHELL_LABEL_ONLY_YOUR_OWN_SETTINGS       _("Only your own settings can be reset - the others need\nthe control centre to be running as root.")
#define STR_SHELL_LABEL_ONLY_YOUR_OWN_SETTINGS_TIP   _("")
#define STR_SHELL_LABEL_FONT                         _("Font")
#define STR_SHELL_LABEL_FONT_TIP                     _("")
#define STR_SHELL_LABEL_APPLIES_ONCE_USE_DEFAULT     _("Applies at once. Use Default returns to GTK's own font.")
#define STR_SHELL_LABEL_APPLIES_ONCE_USE_DEFAULT_TIP _("")
#define STR_SHELL_LABEL_BEHAVIOUR                    _("Behaviour")
#define STR_SHELL_LABEL_BEHAVIOUR_TIP                _("")
#define STR_SHELL_LABEL_OPEN                         _("Open on:")
#define STR_SHELL_LABEL_OPEN_TIP                     _("")
#define STR_SHELL_LABEL_ALSO_WHERE_OK_RETURNS        _("Also where OK returns to. Status is the default because it\nsays whether anything is wrong; Volume is what most people\nopen this for.")
#define STR_SHELL_LABEL_ALSO_WHERE_OK_RETURNS_TIP    _("")
#define STR_SHELL_LABEL_DEFAULT_WHEN_RUNNING_FROM    _("On by default when running from an unpacked folder, where\nseeing what loaded is the point. Off when installed.")
#define STR_SHELL_LABEL_DEFAULT_WHEN_RUNNING_FROM_TIP _("")
#define STR_SHELL_FRAME_STARTUP                      _("Startup")
#define STR_SHELL_FRAME_STARTUP_TIP                  _("")
#define STR_SHELL_FRAME_AFTER_LOAD_UNLOAD            _("After Load and Unload")
#define STR_SHELL_FRAME_AFTER_LOAD_UNLOAD_TIP        _("")
#define STR_SHELL_BTN_COPY_OLD_SETTINGS              _("Copy old settings")
#define STR_SHELL_BTN_COPY_OLD_SETTINGS_TIP          _("")
#define STR_SHELL_BTN_USE_NEW_DEFAULTS               _("Use new defaults")
#define STR_SHELL_BTN_USE_NEW_DEFAULTS_TIP           _("")
#define STR_SHELL_BTN_SAVE_QUIT                      _("Save & Quit")
#define STR_SHELL_BTN_SAVE_QUIT_TIP                  _("")
#define STR_SHELL_LABEL_FILES_OF_TYPE                _("Files of type:")
#define STR_SHELL_LABEL_FILES_OF_TYPE_TIP            _("")
#define STR_SHELL_BTN_DISCARD_QUIT                   _("Discard & Quit")
#define STR_SHELL_BTN_DISCARD_QUIT_TIP               _("")
#define STR_SHELL_BTN_CANCEL                         _("Cancel")
#define STR_SHELL_BTN_CANCEL_TIP                     _("")
#define STR_SHELL_BTN_OK                             _("OK")
#define STR_SHELL_BTN_OK_TIP                         _("")
#define STR_SHELL_BTN_REVERT_SAVED                   _("Revert to Saved")
#define STR_SHELL_BTN_REVERT_SAVED_TIP               _("")
#define STR_SHELL_BTN_RESET                          _("Reset")
#define STR_SHELL_BTN_RESET_TIP                      _("")
#define STR_SHELL_BTN_CLOSE                          _("Close")
#define STR_SHELL_BTN_CLOSE_TIP                      _("")
#define STR_SHELL_BTN_RESET_2                        _("Reset...")
#define STR_SHELL_BTN_RESET_2_TIP                    _("")
#define STR_SHELL_BTN_USE_DEFAULT                    _("Use Default")
#define STR_SHELL_BTN_USE_DEFAULT_TIP                _("")
#define STR_SHELL_BTN_HELP                           _("Help")
#define STR_SHELL_BTN_HELP_TIP                       _("Show the help for this page")
#define STR_SHELL_BTN_MODIFY                         _("Modify...")
#define STR_SHELL_BTN_MODIFY_TIP                     _("")
#define STR_SHELL_BTN_APPLY                          _("Apply")
#define STR_SHELL_BTN_APPLY_TIP                      _("")
#define STR_SHELL_CHECK_SHOW_WHAT_HAPPENED_STEP      _("Show what happened, step by step")
#define STR_SHELL_CHECK_SHOW_WHAT_HAPPENED_STEP_TIP  _("")
#define STR_SHELL_MENU_CURRENT_USER_SETTINGS         _("Current user settings")
#define STR_SHELL_MENU_SYSTEM_SETTINGS               _("System settings")
#define STR_SHELL_MENU_ALL_USERS_SETTINGS            _("All users\' settings")
#define STR_SHELL_MENU_ALL_SETTINGS_SYSTEM_EVERY     _("All settings - system and every user")
#define STR_SHELL_MENU_SAVE_CONFIGURATION            _("Save Configuration")
#define STR_SHELL_MENU_SAVE_DRAFT                    _("Save Draft...")
#define STR_SHELL_MENU_LOAD_CONFIGURATION            _("Load Configuration...")
#define STR_SHELL_MENU_RESET_SETTINGS                _("Reset Settings...")
#define STR_SHELL_MENU_RESTORE_BACKUP                _("Restore Backup...")
#define STR_SHELL_MENU_REVERT_SAVED                  _("Revert to Saved...")
#define STR_SHELL_MENU_PREFERENCES                   _("Preferences...")
#define STR_SHELL_MENU_EXIT                          _("Exit")
#define STR_SHELL_MENU_FILE                          _("File")
#define STR_SHELL_MENU_RESET_WINDOW_SIZE             _("Reset Window Size")
#define STR_SHELL_MENU_VIEW                          _("View")
#define STR_SHELL_MENU_HELP_CONTENTS                 _("Contents...")
#define STR_SHELL_MENU_ABOUT                         _("About VLHE...")
/* THE HELP WINDOW AND THE ABOUT BOX - design/54 G01. The help TEXT is
 * doc/vlhe-help.txt, not here: its headings name the pages and tabs
 * in English, and the Help button matches them against the labels
 * above, so a translation would need a translated help file too. */
#define STR_HELP_TITLE                               _("VLHE Help")
#define STR_HELP_GROUP                               _("Using the control centre")
#define STR_HELP_NOT_FOUND                           _("The help file, vlhe-help.txt, was not found.\n\nLooked in:\n\n")
#define STR_ABOUT_TITLE                              _("About VLHE")
#define STR_ABOUT_NAME                               _("VLHE %s")
#define STR_ABOUT_DESC                               _("Virtual Legacy Hardware Emulation - the control centre")
#define STR_ABOUT_COPYRIGHT                          _("Copyright (c) 2026 Thomas Tranter")
#define STR_ABOUT_LICENCE                            _("Distributed under the BSD 3-Clause licence; see LICENSE.TXT.")
#define STR_ABOUT_BUILD                              _("Built: %s")
#define STR_ABOUT_INSTALLED                          _("Running: installed")
#define STR_ABOUT_PORTABLE                           _("Running: portable, from %s")
#define STR_ABOUT_HELP_FILE                          _("Help file: %s")
#define STR_ABOUT_HELP_NONE                          _("Help file: not found")
#define STR_SHELL_TITLE_START_FROM_YOUR_SETTINGS     _("Start from your settings?")
#define STR_SHELL_TITLE_UNSAVED_CHANGES              _("Unsaved changes")
#define STR_SHELL_TITLE_COULD_NOT_LOAD               _("Could not load")
#define STR_SHELL_TITLE_COULD_NOT_SAVE               _("Could not save")
#define STR_SHELL_TITLE_REVERT_SAVED                 _("Revert to saved")
#define STR_SHELL_TITLE_RESET_SETTINGS               _("Reset Settings")
#define STR_SHELL_TITLE_AUTHENTICATION               _("Authentication")
#define STR_SHELL_TITLE_PREFERENCES                  _("Preferences")
#define STR_SHELL_FRAME_SYSTEM_BASELINE              _("System Baseline")
#define STR_SHELL_LABEL_BASELINE_DRIFT               _("When device files VLHE manages have changed since the baseline was recorded:")
#define STR_SHELL_RADIO_DRIFT_ASK                    _("Ask what to do (default)")
#define STR_SHELL_RADIO_DRIFT_ASK_TIP                _("Show the choices each time: undo the changes, accept them as the new baseline, load anyway, or cancel.")
#define STR_SHELL_RADIO_DRIFT_WARN                   _("Warn and Load")
#define STR_SHELL_RADIO_DRIFT_WARN_TIP               _("Keep the baseline, leave the changes as they are, load, and say what had changed.")
#define STR_SHELL_RADIO_DRIFT_REFUSE                 _("Refuse to Load")
#define STR_SHELL_RADIO_DRIFT_REFUSE_TIP             _("Stop the Load with nothing changed, and say what had changed.")
#define STR_SHELL_LABEL_BASELINE_NEEDS_ROOT          _("A setting for the whole machine - it can be changed when running as root.")
#define STR_SHELL_TEXT_CONFIG_FILE_NOT_ROOT          _("The config\nfile is not\nroot's, so its\nsettings are\nnot in use.\n\nFile > Load\nConfiguration\nreviews it.")
#define STR_SHELL_TEXT_NOT_RUNNING_AS_ROOT           _("Not running\nas root.\n\nSettings can be\nchanged and\nsaved, but the\nmodules cannot\nbe loaded.\n\nStart this as\nroot to load\nthem.")
#define STR_SHELL_TEXT_YOUR_SETTINGS_ARE_SAVED       _("Your settings are\nsaved in your\nhome directory -\nthe portable\nfolder cannot\nbe written.")
#define STR_SHELL_TEXT_YOUR_SETTINGS_ARE_TMP         _("Your settings are\nin /tmp and WILL\nNOT survive a\nreboot - neither\nthe portable\nfolder nor your\nhome directory\ncould be written.")
#define STR_SHELL_TEXT_CONFIGURATION_FILE_WAS_REMOVED _("The configuration\nfile was removed\nwhile this was\nrunning.\n\nYour settings are\nstill loaded here.\n\nFile / Save\nConfiguration\nwrites them back.")
#define STR_SHELL_MSG_NOTHING_COPY_STARTING_FROM     _("Nothing to copy - starting from defaults")
#define STR_SHELL_MSG_YOUR_INSTALLED_SETTINGS_WERE   _("Your installed settings were copied into this folder")
#define STR_SHELL_MSG_THERE_NO_BACKUP_RESTORE        _("There is no backup to restore - one is kept from each Save, as the system file's .old")
#define STR_SHELL_MSG_RESTORING_BACKUP_WRITES_SYSTEM _("Restoring the backup writes the system file, which needs root")
#define STR_SHELL_MSG_CONFIGURATION_RESTORED_FROM_BACKUP _("Configuration restored from the backup")
#define STR_SHELL_MSG_NO_CHANGES                     _("No changes")
#define STR_SHELL_MSG_APPLIED_FILE_SAVE_CONFIGURATION _("Applied - File > Save Configuration to keep it")
#define STR_SHELL_MSG_NOTHING_UNDO_PAGE_FILE         _("Nothing to undo on this page - File > Revert to Saved undoes applied changes")
#define STR_SHELL_MSG_CHANGES_PAGE_UNDONE_BACK       _("Changes on this page undone, back to what was applied")
#define STR_SHELL_MSG_NOTHING_REVERT_EVERYTHING_AS   _("Nothing to revert - everything is as saved")
#define STR_SHELL_MSG_REVERTED_SAVED_CONFIGURATION   _("Reverted to the saved configuration")
#define STR_SHELL_MSG_NO_FILE_CHOSEN                 _("No file chosen")
#define STR_SHELL_MSG_NOTHING_WAS_LOADED             _("Nothing was loaded")
#define STR_SHELL_MSG_FONT_RESET_INTERFACE_DEFAULT   _("Font reset to the interface default - File > Save Configuration to keep it")
#define STR_SHELL_MSG_FONT_APPLIED_FILE_SAVE         _("Font applied - File > Save Configuration to keep it")
#define STR_SHELL_MSG_NO_FONT_SET_INTERFACE          _("No font set - the interface font is unchanged")
#define FMT_SHELL_MSG_BASELINE_SET                   _("System Baseline set to %s - File > Save Configuration to keep it")
#define FMT_SHELL_MSG_FONT_AND_BASELINE_SET          _("Font applied and System Baseline set to %s - File > Save Configuration to keep them")
#define STR_SHELL_DRIFT_NAME_ASK                     _("Ask what to do")
#define STR_SHELL_DRIFT_NAME_WARN                    _("Warn and Load")
#define STR_SHELL_DRIFT_NAME_REFUSE                  _("Refuse to Load")
#define STR_SHELL_MSG_COULD_NOT_APPLY                _("Could not apply")
#define STR_SHELL_MSG_NEEDS_ROOT_RUN_CONTROL         _("That needs root - run the control centre as root")
#define STR_SHELL_MSG_NOTHING_NEEDED_RESETTING_NO    _("Nothing needed resetting - no settings files were found in what you chose")
#define STR_SHELL_MSG_UNLOCKED_SETTINGS_CAN_NOW      _("Unlocked - settings can now be applied")
#define STR_SHELL_MSG_NO_CONFIGURATION_FILE_SETTINGS _("No configuration file - settings apply now and are kept by File > Save Configuration")
#define STR_SHELL_MSG_WINDOW_RESET_800_X             _("Window reset to 800 x 600")
#define STR_SHELL_MSG_THERE_ARE_UNSAVED_MACHINE      _("there are unsaved machine settings - save or cancel them first")
#define FMT_SHELL_VIEWER_NOT_BUILT_YET               _("%s\n\n(viewer not built yet)")
#define FMT_SHELL_NOT_BUILT_YET                      _("%s\n\n(not built yet)")
#define FMT_SHELL_SOME_SETTINGS_HAVE_BEEN            _("Some settings have been changed but not applied")
#define FMT_SHELL_LOAD_WILL_USE_APPLIED              _("\n\nThe load will use the applied settings.")
#define FMT_SHELL_SOME_SETTINGS_USES_HAVE            _("Some settings the %.24s uses have been changed but not applied")
#define FMT_SHELL_RESTART_WILL_USE_APPLIED           _("\n\nThe restart will use the applied settings.")
#define FMT_SHELL_SETTINGS_RENDER_USES_HAVE          _("Settings the render uses have been changed but not applied")
#define FMT_SHELL_RENDER_WILL_USE_APPLIED            _("\n\nThe render will use the applied settings.")
#define FMT_SHELL_RUNNING_AS_ROOT_LOCKED             _("Running as %.32s\n(root locked -\nModify unlocks).")
#define FMT_SHELL_RUNNING_AS_UNLOCKED_AS             _("Running as %.32s,\nunlocked as root.")
#define FMT_SHELL_RUNNING_AS_ROOT                    _("Running as root.")
#define FMT_SHELL_RUNNING_AS                         _("Running as %.32s.")
#define FMT_SHELL_MACHINE_SETTINGS_ARE_FROM          _("Machine settings\nare from your\ndraft of\n%.16s.\n\nThe system runs\nits own until\nroot imports it\n(File > Load\nConfiguration).")
#define FMT_SHELL_COPY_RUNS_FROM_OWN                 _("This copy runs from its own folder and keeps its\nsettings there. VLHE is also installed on this\nmachine.\n\nStart from the installed settings, or from the\ndefaults?\n\nEither way this copy keeps its own settings in its\nfolder, and nothing installed is changed.")
#define FMT_SHELL_COULD_NOT_COPY                     _("Could not copy: %.180s")
#define FMT_SHELL_SOME_SETTINGS_HAVE_BEEN_2          _("Some settings have been changed but not saved")
#define FMT_SHELL_PUT_BACKUP_OLD_BACK                _("Put the backup\n\n    %.*s.old\n%s%s%s\nback in place of the current system configuration?\n\nThe settings now in memory are replaced by the backup's; nothing is written anywhere else.")
#define FMT_SHELL_COULD_NOT_LOAD                     _("Could not load\n\n    %.*s\n\n%.*s")
#define FMT_SHELL_COULD_NOT_WRITE                    _("Could not write\n\n    %.*s\n\n%.*s")
#define FMT_SHELL_SETTINGS_WERE_NOT_ACCEPTED         _("%.40s: its settings were not accepted - see the page")
#define FMT_SHELL_FOLLOWING_PAGES_HAVE_UNSAVED       _("The following pages have unsaved changes:\n")
#define FMT_SHELL_SETTINGS_ALREADY_APPLIED           _("\n    (settings already applied)")
#define FMT_SHELL_WOULD_YOU_LIKE_DISCARD             _("\n\nWould you like to discard them and revert to saved?")
#define FMT_SHELL_DRAFT_WRITTEN_ROOT_CAN             _("Draft written to %.200s - root can open it with File > Load Configuration")
#define FMT_SHELL_NOTHING_WOULD_CHANGE_MACHINE       _("Nothing in it would change this machine's settings.\n")
#define FMT_SHELL_MORE_NOT_SHOWN                     _("    ...and %d more, not shown\n")
#define FMT_SHELL_REFUSED_NOT_VALUE_THESE            _("\nRefused - not a value these pages offer:\n")
#define FMT_SHELL_SKIPPED_NEVER_TAKEN_FROM           _("\nSkipped - never taken from a file (paths, modules, settings no page shows):\n")
#define FMT_SHELL_USER_S_OWN_SETTINGS                _("\n%d of the user's own settings (fonts, folders, recent images) ignored.\n")
#define FMT_SHELL_CHANGE_S_LOADED_AS                 _("%d change(s) loaded as unsaved edits - %s")
#define FMT_SHELL_SETTINGS_ARE_NOT_USE               _("%.*s.\n\nIts settings are not in use: this machine is running on the defaults until they are reviewed.\n\nReview them and take them over? Accept makes the differences unsaved changes; Save then writes a copy of root's own.")
#define FMT_SHELL_COULD_NOT_WRITE_YOUR               _("Could not write your settings to\n\n    %.*s\n\n%.*s\n\nSave them to your home directory instead?\n\n    %.*s\n\nThey will be found there next time.")
#define FMT_SHELL_COULD_NOT_WRITE_YOUR_2             _("Could not write your settings to\n\n    %.*s\n\n%.*s\n\nThey will be kept in\n\n    %.*s\n\nThis is temporary: they will NOT survive a\nreboot.")
#define FMT_SHELL_SAVED_YOUR_MACHINE_SETTINGS        _("Saved. Your machine settings went to %.*s - root imports them with File > Load Configuration")
#define FMT_SHELL_RESET_DEFAULTS_EACH_FILE           _("Reset %.60s to the defaults?\n\nEach file is renamed to <name>.old first, so this can be\nundone by renaming it back.\n\n%s")
#define STR_SHELL_TITLE_SETTINGS_NOT_APPLIED         _("Settings not applied")
#define STR_SHELL_BTN_APPLY_LOAD                     _("Apply & Load")
#define STR_SHELL_BTN_APPLY_LOAD_TIP                 _("")
#define STR_SHELL_BTN_LOAD_WITHOUT_APPLYING          _("Load without Applying")
#define STR_SHELL_BTN_LOAD_WITHOUT_APPLYING_TIP      _("")
#define STR_SHELL_BTN_APPLY_RESTART                  _("Apply & Restart")
#define STR_SHELL_BTN_APPLY_RESTART_TIP              _("")
#define STR_SHELL_BTN_RESTART_WITHOUT_APPLYING       _("Restart without Applying")
#define STR_SHELL_BTN_RESTART_WITHOUT_APPLYING_TIP   _("")
#define STR_SHELL_BTN_APPLY_RENDER                   _("Apply & Render")
#define STR_SHELL_BTN_APPLY_RENDER_TIP               _("")
#define STR_SHELL_BTN_RENDER_WITHOUT_APPLYING        _("Render without Applying")
#define STR_SHELL_BTN_RENDER_WITHOUT_APPLYING_TIP    _("")
#define STR_SHELL_BTN_ACCEPT_MAKES_THESE_UNSAVED     _("Accept makes these unsaved changes; nothing is written until Save.")
#define STR_SHELL_BTN_ACCEPT_MAKES_THESE_UNSAVED_TIP _("")
#define STR_SHELL_BTN_ACCEPT                         _("Accept")
#define STR_SHELL_BTN_ACCEPT_TIP                     _("")
#define STR_SHELL_TITLE_RESTORE_BACKUP               _("Restore backup")
#define STR_SHELL_BTN_RESTORE                        _("Restore")
#define STR_SHELL_BTN_RESTORE_TIP                    _("")
#define STR_SHELL_TITLE_CONFIGURATION_NOT_USE        _("Configuration not in use")
#define STR_SHELL_BTN_REVIEW                         _("Review")
#define STR_SHELL_BTN_REVIEW_TIP                     _("")
#define STR_SHELL_BTN_NOT_NOW                        _("Not now")
#define STR_SHELL_BTN_NOT_NOW_TIP                    _("")
#define STR_SHELL_TITLE_SETTINGS_COULD_NOT_SAVED     _("Settings could not be saved")
#define STR_SHELL_BTN_YES_USE_MY_HOME                _("Yes, use my home directory")
#define STR_SHELL_BTN_YES_USE_MY_HOME_TIP            _("")
#define STR_SHELL_BTN_NO                             _("No")
#define STR_SHELL_BTN_NO_TIP                         _("")
#define STR_SHELL_TITLE_SETTINGS_KEPT_TMP            _("Settings kept in /tmp")


/* ==================================================================
 * ITS DIALOGS - Preferences, Modify (the password prompt), the quit
 * prompt, before Load / Restart / Render, Save Draft and Load
 * Configuration, Reset Settings, Restore Backup, Revert to Saved.
 * vlhe_cc.c.
 * ================================================================== */

/* ==================================================================
 * STATUS - vlhe_mod_status.c. Load, Unload, Simulate, the result box.
 * ================================================================== */

/* #define STR_STATUS_LABEL_PAGE_NOTE                   _("Load brings up the components ticked above; Unload removes all loaded components. Simulate shows what they would run, running nothing.\n\nRestarting a daemon applies its settings and fixes a wedged one, and interrupts whatever it is playing.") */
#define STR_STATUS_LABEL_PAGE_NOTE                   _("")
#define STR_STATUS_LABEL_PAGE_NOTE_TIP               _("")
#define FMT_STATUS_BOX_COMPONENT_FAILED              _("  ---   that component failed; continuing with the others\n")


#define STR_STATUS_TEXT_NOT_LOADED                   _("not loaded")
#define STR_STATUS_TEXT_NOT_RUNNING                  _("not running")
#define FMT_STATUS_NO_SOUNDFONT_SET                  _("No SoundFont is set - vmidid will not play.  See Midi Settings.")
#define STR_STATUS_SONG_FONT_NO_GM                   _("A song font is set but no General MIDI font - vmidid will not play.  See Midi Settings.")
#define STR_STATUS_TEXT_SOMETHING_ELSE               _("something else")
#define STR_STATUS_TEXT_DONE_MACHINE_AS_WAS          _("\nDone - the machine is as it was.\n")
#define STR_STATUS_TEXT_NEEDS_ROOT_MODIFY            _("This needs root - press Modify to unlock it")
#define STR_STATUS_TEXT_NEEDS_ROOT_START_AS_ROOT     _("This needs root - start the control centre as root")
#define STR_STATUS_MSG_NOTHING_TO_UNLOAD             _("nothing to unload")
#define STR_STATUS_MSG_NOTHING_TO_LOAD               _("nothing to load - check what is enabled")
#define STR_STATUS_MSG_UNLOADED_AS_IT_WAS            _("unloaded - the machine is as it was")
#define FMT_STATUS_TEXT_DONE_BASELINE_DIFFERS        _("\nDone - VLHE's own changes are undone, but %d path(s) differ from the baseline (above).\n")
#define FMT_STATUS_MSG_UNLOADED_BASELINE_DIFFERS     _("unloaded - %d path(s) differ from the baseline")
/* The unload's outcome when it is NOT "as it was" - the journal's verdict
 * in the box's words (design/54 7h, 86Box 2026-10-03). */
#define FMT_STATUS_TEXT_DONE_STILL_IN_EFFECT         _("\nDone - %d change(s) still in effect; the machine is not as it was. See the Status page.\n")
#define FMT_STATUS_MSG_UNLOADED_STILL_IN_EFFECT      _("unloaded - %d change(s) still in effect, the machine is not as it was")
#define FMT_STATUS_TEXT_DONE_CONFLICT                _("\nDone - %d change(s) left as someone else changed them (CONFLICT, above); the machine is not exactly as it was.\n")
#define FMT_STATUS_MSG_UNLOADED_CONFLICT             _("unloaded - %d CONFLICT(s) left as found, the machine is not exactly as it was")
#define STR_STATUS_TEXT_DONE_SESSION_UNREADABLE      _("\nDone - the session file could not be read, so whether anything is still in effect is not known.\n")
#define STR_STATUS_MSG_UNLOADED_SESSION_UNREADABLE   _("unloaded - the session file could not be read")
#define STR_STATUS_FILTER_TEXT                       N_("Text files (*.txt)")
#define STR_STATUS_FILTER_ALL                        N_("All files (*)")
#define STR_STATUS_TITLE_SAVE_SIMULATION             _("Save the simulation")


/* The Include-in-load, diagnostics and flood checkboxes are built
 * from tables, so their labels are N_() and their tips are set with
 * vlhe_tip() after the loop; the main Load button shares its label
 * with the per-component one and has its own tip. */
#define STR_STATUS_CHECK_INCLUDE_SOUND               N_("Sound")
#define STR_STATUS_CHECK_INCLUDE_SOUND_TIP           _("Load vsound and the mixing pump.")
#define STR_STATUS_CHECK_INCLUDE_MIDI                N_("MIDI")
#define STR_STATUS_CHECK_INCLUDE_MIDI_TIP            _("Load vmidi and the synth. Turn off to test the rest without it.")
#define STR_STATUS_CHECK_INCLUDE_CD                  N_("CD")
#define STR_STATUS_CHECK_INCLUDE_CD_TIP              _("Load vdisc and the disc server.")
#define STR_STATUS_BTN_LOAD_MAIN                     N_("Load Checked")
#define STR_STATUS_BTN_LOAD_MAIN_TIP                 _("Load the components ticked above and start their daemons.")


#define STR_STATUS_LABEL_INCLUDE_LOAD                _("Include in load:")
#define STR_STATUS_LABEL_INCLUDE_LOAD_TIP            _("")
#define STR_STATUS_FRAME_COMPONENTS                  _("Components")
#define STR_STATUS_FRAME_COMPONENTS_TIP              _("")
#define STR_STATUS_BTN_SAVE                          _("Save...")
#define STR_STATUS_BTN_SAVE_TIP                      _("")
#define STR_STATUS_BTN_LOAD                          _("Load")
#define STR_STATUS_BTN_LOAD_TIP                      _("Load just this component, leaving the others as they are")
#define STR_STATUS_BTN_APPLY_SETTINGS                _("Apply settings")
#define STR_STATUS_BTN_APPLY_SETTINGS_TIP            _("Make it use the applied settings now, without restarting it")
#define STR_STATUS_BTN_RESTART                       _("Restart")
#define STR_STATUS_BTN_RESTART_TIP                   _("Stop and start it - the audio it is producing will break")
#define STR_STATUS_BTN_UNLOAD                        _("Unload All")
#define STR_STATUS_BTN_UNLOAD_TIP                    _("Stop the daemons and remove the modules, putting the sound card back where it was.")
#define STR_STATUS_BTN_SIMULATE                      _("Simulate")
#define STR_STATUS_BTN_SIMULATE_TIP                  _("Show every command Load would run for the boxes ticked, and what Unload would then undo - with nothing run.")
#define STR_STATUS_CHECK_EXPLAIN_EACH_STEP           _("Explain each step")
#define STR_STATUS_CHECK_EXPLAIN_EACH_STEP_TIP       _("")
#define STR_STATUS_TEXT_LOADED                       _("loaded")
#define STR_STATUS_MSG_CANCELLED_NOTHING_WAS_LOADED  _("cancelled - nothing was loaded or unloaded")
/* THE UNFINISHED-UNLOAD BANNER - design/54 7h Stage 2c. */
#define STR_STATUS_FRAME_LEFTOVER                    _("Unfinished unload")
#define FMT_STATUS_LEFTOVER_TEXT                     _("The last load was not unloaded (a shutdown or power loss?). %d change(s) it made are still on record, for %s. Load is refused until the unload is finished.")
#define FMT_STATUS_LEFTOVER_FAILED_TEXT              _("The last unload could not undo %d change(s), for %s. Load is refused until the unload is finished.")
#define FMT_STATUS_LEFTOVER_MIXED_TEXT               _("The last load was not fully unloaded. %d change(s) are still on record, for %s: %d the last unload could not undo, the rest never undone (a shutdown or power loss?). Load is refused until the unload is finished.")
#define STR_STATUS_NOTICE_TITLE                      _("At start-up")
#define STR_STATUS_NOTICE_HEAD                       _("The last start-up had something to report (shown once):\n\n")
#define FMT_STATUS_NOTICE_FOOT                       _("\nThe full account is in %s.")
#define STR_STATUS_BTN_FINISH_UNLOAD                 _("Finish the unload")
#define STR_STATUS_BTN_FINISH_UNLOAD_TIP             _("Put back what the last load changed: stop and unload what is left of it and restore the device nodes. Anything someone else changed since is left as found.")
#define STR_STATUS_BTN_NOT_NOW                       _("Not now")
#define STR_STATUS_BTN_NOT_NOW_TIP                   _("Hide this until the next Load. Load stays refused until the unload is finished.")
#define STR_STATUS_MSG_LOAD_REFUSED_LEFTOVER         _("Load refused - the last load was not unloaded. Finish the unload first (top of this page). Nothing was changed.")
#define STR_STATUS_MSG_FINISHING                     _("finishing the unload...")
#define STR_SHELL_DRIFT_TITLE                        _("Changed since VLHE first saw it")
#define STR_SHELL_DRIFT_HEAD                         _("%d path(s) have changed since VLHE first saw them, and VLHE did not change them:\n")
#define STR_SHELL_DRIFT_TAIL                         _("\n\nUndo changes puts them back as they were, then loads. Accept changes makes this the new baseline, then loads. Load anyway leaves them and asks again next time.")
#define STR_SHELL_DRIFT_UNDO                         _("Undo changes")
#define STR_SHELL_DRIFT_UNDO_TIP                     _("Put these paths back as they were before VLHE first changed them, then load.")
#define STR_SHELL_DRIFT_ACCEPT                       _("Accept changes")
#define STR_SHELL_DRIFT_ACCEPT_TIP                   _("Make what is there now the baseline (the old one is kept beside it), then load.")
#define STR_SHELL_DRIFT_LOAD                         _("Load anyway")
#define STR_SHELL_DRIFT_LOAD_TIP                     _("Load, leaving the changes and the baseline as they are. You will be asked again next time.")
#define STR_SHELL_DRIFT_CANCEL                       _("Cancel")
#define STR_SHELL_DRIFT_CANCEL_TIP                   _("Load nothing and change nothing.")
#define STR_STATUS_MSG_LOAD_CANCELLED_DRIFT          _("Load cancelled - nothing was changed")
#define STR_STATUS_MSG_LOAD_REFUSED_DRIFT            _("Load refused - device files VLHE manages have changed since the baseline was recorded, and System Baseline (File, Preferences) is set to Refuse to Load. Nothing was changed.")
#define FMT_STATUS_SEE_JOURNAL                       _("%.400s (the full account is in %.250s)")
/* WHAT NEEDS ATTENTION - design/54 7h Stage 3.5c. The frame keeps the
 * leftover's own title when that is all it holds. */
#define STR_STATUS_FRAME_ATTENTION                   _("Needs attention")
#define FMT_STATUS_FIND_DSP                          _("%s points to %s. It may have been left by an earlier VLHE load, or it may be your own setup.")
#define FMT_STATUS_FIND_CDROM                        _("%s points to %s, which no longer exists. It was left by an earlier VLHE load.")
#define FMT_STATUS_FIND_CDROM_WAS                    _("%s points to %s, which no longer exists. It was left by an earlier VLHE load; before that it pointed to %s.")
#define STR_STATUS_BTN_RESTORE_STOCK                 _("Restore stock node")
#define STR_STATUS_BTN_RESTORE_STOCK_TIP             _("Put back the device node a stock machine has here, and remember it as how this machine was.")
#define STR_STATUS_BTN_KEEP_IT                       _("Keep it")
#define STR_STATUS_BTN_KEEP_IT_TIP                   _("It is your own setup: leave it, and remember it as how this machine was.")
#define STR_STATUS_BTN_POINT_DRIVE                   _("Point at drive")
#define STR_STATUS_BTN_POINT_DRIVE_TIP               _("Point /dev/cdrom at the drive chosen beside this button.")
#define STR_STATUS_BTN_REMOVE_LINK                   _("Remove link")
#define STR_STATUS_BTN_REMOVE_LINK_TIP               _("Remove /dev/cdrom - for a machine that had none before.")
#define STR_STATUS_TEXT_TYPE_DRIVE                   _("No CD drive was found. Type the device /dev/cdrom should point at, for example /dev/hdc.")
#define FMT_STATUS_ATTENTION_COUNT                   _("%d thing(s) need attention.")
#define STR_STATUS_BTN_REVIEW                        _("Review...")
#define STR_STATUS_BTN_REVIEW_TIP                    _("Show each one, and what can be done about it.")
#define STR_STATUS_MSG_FINISH_BUSY                   _("another Load or Unload is running - try again when it has finished")
#define STR_STATUS_MSG_COULD_NOT_SAVE_SIMULATION     _("Could not save the simulation")
#define STR_STATUS_MSG_SIMULATION_SAVED              _("Simulation saved")
#define STR_STATUS_MSG_COULD_NOT_WRITE_SIMULATION    _("Could not write the simulation")
#define STR_STATUS_MSG_NOTHING_LOAD_COMPONENT        _("nothing to load for this component")
#define STR_STATUS_MSG_SETTINGS_APPLIED_SAMPLE_RATE  _("Settings applied. The sample rate takes effect at the next pause in the music.")
#define FMT_STATUS_EVERYTHING_LOADED_RUNNING         _("Everything is loaded and running.")
#define FMT_STATUS_ONE_COMPONENT_NOT_RUNNING         _("One component is not running - see below.")
#define FMT_STATUS_COMPONENTS_ARE_NOT_RUNNING        _("%d components are not running - see below.")
#define FMT_STATUS_DEV_CDROM_WAS                     _("/dev/cdrom -> %.100s  (was %.100s)")
#define FMT_STATUS_DEV_CDROM                         _("/dev/cdrom -> %.100s")
#define FMT_STATUS_DEV_CDROM_NOT_OURS                _("/dev/cdrom -> %.100s  -  not ours any more (we saved %.100s)")
#define FMT_STATUS_ONE_COMPONENT_FAILED_SEE          _("\nOne component failed - see above.  The others were not affected.\n")
#define FMT_STATUS_COMPONENTS_FAILED_SEE_ABOVE       _("\n%d components failed - see above.\n")
#define FMT_STATUS_ONE_COMPONENT_FAILED              _("one component failed: %.240s")
#define FMT_STATUS_ONE_COMPONENT_FAILED_SEE_2        _("one component failed - see the details")
#define FMT_STATUS_COMPONENTS_FAILED                 _("%d components failed: %.240s")
#define FMT_STATUS_COMPONENTS_FAILED_SEE_DETAILS     _("%d components failed - see the details")
#define FMT_STATUS_NOT_RUNNING_NOTHING_APPLY         _("%.32s is not running - nothing to apply to")
#define FMT_STATUS_SETTING_S_WERE_REFUSED            _("%d setting(s) were refused - see the log")
#define FMT_STATUS_COULD_NOT_RESTART                 _("Could not restart %.32s")
#define FMT_STATUS_RESTARTED                         _("%.32s restarted")
#define FMT_STATUS_WILL_LOADED_FILE_SAVE             _("%s will %sbe loaded - File > Save Configuration to keep it.")


/* ==================================================================
 * VOLUME - vlhe_mod_volume.c. Tabs: Levels, Options.
 * ================================================================== */

#define STR_VOL_LABEL_MUTE                           _("Mute")
#define STR_VOL_LABEL_MUTE_TIP                       _("")
#define STR_VOL_LABEL_BOOST                          _("Boost")
#define STR_VOL_LABEL_BOOST_TIP                      _("Lets this channel go above 100%, to bring up a quiet program. The limiter keeps the mix from clipping.")
#define STR_VOL_LABEL_THESE_ARE_PER_PROGRAM          _("These are per-program levels. The card's own PCM and Master control everything above.")
#define STR_VOL_LABEL_THESE_ARE_PER_PROGRAM_TIP      _("")
#define STR_VOL_LABEL_VSOUND_MODULE_NOT_LOADED       _("The vsound module is not loaded.\n\nPer-program volume is unavailable until it is.")
#define STR_VOL_LABEL_VSOUND_MODULE_NOT_LOADED_TIP   _("")
#define STR_VOL_LABEL_NOTHING_USING_VSOUND_YET       _("Nothing is using vsound yet.\n\nStart a program that plays sound - a game, the CD player, the synth - and its volume appears here.")
#define STR_VOL_LABEL_NOTHING_USING_VSOUND_YET_TIP   _("")
#define STR_VOL_LABEL_OFF_DRAWS_FADERS_HORIZONTALLY  _("Off draws the faders horizontally, which fits more channels in the same height.")
#define STR_VOL_LABEL_OFF_DRAWS_FADERS_HORIZONTALLY_TIP _("")
#define STR_VOL_LABEL_CARD_S_LEVELS_RESET            _("The card's levels reset whenever the sound modules are loaded, not only at boot. With this on, the levels you set are saved on a clean shutdown and restored afterwards - including a muted microphone.")
#define STR_VOL_LABEL_CARD_S_LEVELS_RESET_TIP        _("")
#define STR_VOL_LABEL_LEVELS                         _("Levels")
#define STR_VOL_LABEL_LEVELS_TIP                     _("")
#define STR_VOL_LABEL_OPTIONS                        _("Options")
#define STR_VOL_LABEL_OPTIONS_TIP                    _("")
#define STR_VOL_TAB_OPTIONS_DIRTY                    _("Options *")
#define STR_VOL_TEXT_CHANNEL_MUTED                   _("muted")
#define STR_VOL_TEXT_CHANNEL_MIDI                    _("MIDI")
#define STR_VOL_FRAME_PLAYBACK_VOLUME                _("Playback Volume")
#define STR_VOL_FRAME_PLAYBACK_VOLUME_TIP            _("")
#define STR_VOL_FRAME_APPEARANCE                     _("Appearance")
#define STR_VOL_FRAME_APPEARANCE_TIP                 _("")
#define STR_VOL_FRAME_CARD_MIXER_LEVELS              _("Card Mixer Levels")
#define STR_VOL_FRAME_PROGRAM_LEVELS                 _("Program Levels")
#define STR_VOL_CHECK_SAVE_PROGRAM_LEVELS            _("Keep each program's level across restarts")
#define STR_VOL_CHECK_SAVE_PROGRAM_LEVELS_TIP        _("Each program's level and mute are saved when VLHE unloads, and at shutdown, and put back when it loads, before any program starts.")
#define STR_VOL_LABEL_PROGRAM_LEVELS_NOTE            _("A program's level is always remembered by its name while the sound module stays loaded - Quake keeps its level across track changes. With this on, it is also kept when the modules are reloaded or the machine restarts.")
#define STR_VOL_FRAME_CARD_MIXER_LEVELS_TIP          _("")
#define STR_VOL_CHECK_VERTICAL_VOLUME_SLIDERS        _("Vertical volume sliders")
#define STR_VOL_CHECK_VERTICAL_VOLUME_SLIDERS_TIP    _("")
#define STR_VOL_CHECK_SAVE_RESTORE_SOUND_CARD        _("Save and restore the sound card's mixer levels")
#define STR_VOL_CHECK_SAVE_RESTORE_SOUND_CARD_TIP    _("Every sound card's levels are saved when VLHE unloads, and at shutdown, and put back when it loads, and at boot.")
#define STR_VOL_MSG_COULD_NOT_SET_VOLUME             _("Could not set volume - the stream may have ended")
#define STR_VOL_MSG_COULD_NOT_CHANGE_MUTE            _("Could not change mute - the stream may have ended")
#define FMT_VOL_FREE                                 _("(free)")
#define STR_VOL_TIP_MUTE_CHANNEL                     _("Mute this channel")


/* ==================================================================
 * SOUND SETTINGS - vlhe_mod_sound.c. Tabs: Device, Options.
 * ================================================================== */

#define STR_SND_TAB_DEVICE_DIRTY                     _("Device *")
#define STR_SND_TAB_OPTIONS_DIRTY                    _("Options *")
#define STR_SND_TAB_DEVICE                           _("Device")
#define STR_SND_TAB_OPTIONS                          _("Options")
#define STR_SND_LABEL_PLAY_THROUGH                   _("Play through:")
#define STR_SND_LABEL_PLAY_THROUGH_TIP               _("")
#define STR_SND_LABEL_PROGRAMS_USE                   _("Programs use:")
#define STR_SND_LABEL_PROGRAMS_USE_TIP               _("")
#define STR_SND_TEXT_WITH                            _("with")
#define STR_SND_TEXT_WITHOUT                         _("without")
#define STR_SND_LABEL_RELEASE_NOTE                   _("Needed for the ESS Solo-1. If you hear a tone when nothing is playing, try this. It should not be needed otherwise.")
#define STR_SND_LABEL_RELEASE_NOTE_TIP               _("")
#define STR_SND_LABEL_MIDI_SLOT_NOTE                 _("Keeps a channel free for MIDI. Without it the synth may find none. Leave it off on a machine that never plays MIDI.")
#define STR_SND_LABEL_MIDI_SLOT_NOTE_TIP             _("")
#define STR_SND_TEXT_NEEDS_ROOT_MODIFY               _("Saving the machine settings on this page needs root. Press Modify to save them now, or File > Save Draft to hand them to root to import.")
#define STR_SND_TEXT_NEEDS_ROOT_RUN_AS_ROOT          _("These are system settings and need root to save. Run the Control Center as root, or use File > Save Draft for root to import.")


#define STR_SND_LABEL_YOUR_PROGRAMS_DEVICE_POINTED   _("Your programs' device is pointed at vsound while the modules are loaded, and put back on unload. Your cards are not moved.")
#define STR_SND_LABEL_YOUR_PROGRAMS_DEVICE_POINTED_TIP _("")
#define STR_SND_LABEL_LIMITER                        _("Limiter:")
#define STR_SND_LABEL_ATTENUATION                    _("Mix level:")
#define STR_SND_MENU_LIMITER_OFF                     _("Off")
#define STR_SND_MENU_LIMITER_ATTACK_RELEASE          _("Attack and release")
#define STR_SND_MENU_LIMITER_SOFT_KNEE               _("Soft knee")
#define STR_SND_MENU_LIMITER_TIP                     _("What happens when programs together are louder than the card can play. Attack and release turns the whole mix down for a loud peak and brings it back; soft knee bends only the loud samples. Off clips.")
#define STR_SND_MENU_ATTEN_NONE                      _("100% (unchanged)")
#define STR_SND_MENU_ATTEN_PCT                       _("%d%%")
#define STR_SND_MENU_ATTEN_TIP                       _("Turns the whole mix down by a fixed amount. 71% is about -3 dB, 50% about -6 dB.")
#define STR_SND_LABEL_HEADROOM_NOTE                  _("Quiet sound passes through untouched. Both take effect the next time VLHE loads.")
#define STR_SND_LABEL_DEVICE                         _("Device")
#define STR_SND_LABEL_DEVICE_TIP                     _("")
#define STR_SND_FRAME_SOUND_CARD                     _("Sound Card")
#define STR_SND_FRAME_SOUND_CARD_TIP                 _("")
#define STR_SND_FRAME_MIXING                         _("Mixing")
#define STR_SND_FRAME_MIXING_TIP                     _("")
#define STR_SND_FRAME_PUMP                           _("Pump")
#define STR_SND_FRAME_PUMP_TIP                       _("")
#define STR_SND_FRAME_MODULE                         _("Module")
#define STR_SND_FRAME_MODULE_TIP                     _("")
#define STR_SND_FRAME_MIX_HEADROOM                   _("Mix Headroom")
#define STR_SND_FRAME_MIX_HEADROOM_TIP               _("")
#define STR_SND_CHECK_RELEASE_CARD_WHEN_LAST         _("Release the card when the last program stops")
#define STR_SND_CHECK_RELEASE_CARD_WHEN_LAST_TIP     _("")
#define STR_SND_CHECK_RESERVE_CHANNEL_MIDI_SYNTH     _("Reserve a channel for the MIDI synth")
#define STR_SND_CHECK_RESERVE_CHANNEL_MIDI_SYNTH_TIP _("")
#define STR_SND_MENU_DETECT                          _("(detect)")
#define STR_SND_MENU_NO_SOUND_CARD_FOUND             _("(no sound card found)")
#define STR_SND_MSG_SOUND_SETTINGS_DEVICE_NAME       _("Sound Settings: a device name was refused - it must be /dev/dsp or /dev/dspN")
#define FMT_SND_VSOUND                               _("%.40s - %.40s (this is vsound)")
#define FMT_SND_VSOUND_2                             _("%.40s (this is vsound)")
#define FMT_SND_USE                                  _(" (in use)")
#define FMT_SND_PROGRAMS_PLAY_ONCE                   _("%d programs play at once.")
#define FMT_SND_VSOUND_NOT_LOADED_APPLIES            _("vsound is not loaded - this applies when it is.")
#define FMT_SND_MODULE_RUNNING_SLOT_RELOAD           _("The module is running %s the slot. Reload vsound to apply.")
#define FMT_SND_MATCHES_RUNNING_MODULE               _("This matches the running module.")


/* ==================================================================
 * MIDI SETTINGS - vlhe_mod_midi.c. Tabs: Sound Fonts, Options,
 * Driver.
 * ================================================================== */

#define STR_MID_TAB_SOUND_FONTS                      N_("Sound Fonts")
#define STR_MID_TAB_OPTIONS                          N_("Options")
#define STR_MID_TAB_DRIVER                           N_("Driver")
#define FMT_MID_NO_SOUNDFONT_IN_FOLDER               _("no SoundFont in %.200s - add it once one is there")
#define STR_MID_TITLE_SELECT_FOLDER                  _("Select a folder holding SoundFonts - open it, or pick a file inside it")
#define STR_MID_MSG_NO_SOUNDFONTS_FOUND              _("No SoundFonts found")
#define STR_MID_MSG_SONG_FONT_NEEDS_GM               _("Choose a General MIDI font first - a song font needs one under it. Nothing changed.")
#define STR_MID_MSG_NO_CHANGE_SAME_FONTS             _("No change - the same fonts are there")
#define FMT_MID_FONT_NOT_THERE_ANY_MORE              _("%.64s is not there any more - the file at %.160s is missing.")
#define STR_MID_LABEL_GENERAL_MIDI                   _("General MIDI:")
#define STR_MID_LABEL_GENERAL_MIDI_TIP               _("")
#define STR_MID_LABEL_SONG_FONT                      _("Song font (bank 1):")
#define STR_MID_LABEL_SONG_FONT_TIP                  _("")
#define STR_MID_LABEL_FOLDER_COLUMN                  _("Folder")
#define STR_MID_LABEL_FOLDER_COLUMN_TIP              _("")
#define STR_MID_LABEL_MAXIMUM_VOICES                 _("Maximum voices:")
#define STR_MID_LABEL_MAXIMUM_VOICES_TIP             _("")
#define STR_MID_LABEL_VOICES_NOTE                    _("(fewer costs less CPU)")
#define STR_MID_LABEL_VOICES_NOTE_TIP                _("")
#define STR_MID_LABEL_MASTER_GAIN                    _("Master gain:")
#define STR_MID_LABEL_MASTER_GAIN_TIP                _("")
#define STR_MID_LABEL_GAIN_NOTE                      _("% (100 = unity; above it clips)")
#define STR_MID_LABEL_GAIN_NOTE_TIP                  _("")
#define STR_MID_LABEL_VOLUME_CURVE                   _("Volume curve:")
#define STR_MID_LABEL_VOLUME_CURVE_TIP               _("")
#define STR_MID_LABEL_VELOCITY_FILTER                _("Velocity filter:")
#define STR_MID_LABEL_VELOCITY_FILTER_TIP            _("")
#define STR_MID_RATE_44100                           N_("44100 Hz (best quality)")
#define STR_MID_RATE_32000                           N_("32000 Hz (a little less work)")
#define STR_MID_RATE_22050                           N_("22050 Hz (half the work)")
#define STR_MID_RATE_11025                           N_("11025 Hz (slowest machines)")
#define STR_MID_LABEL_SAMPLE_RATE                    _("Sample rate:")
#define STR_MID_LABEL_SAMPLE_RATE_TIP                _("")
#define STR_MID_MINOR_7                              N_("7  (the reserved AWFM slot)")
#define STR_MID_MINOR_10                             N_("10 (dmfm)")
#define STR_MID_MINOR_11                             N_("11 (the default)")
#define STR_MID_MINOR_12                             N_("12 (adsp)")
#define STR_MID_MINOR_13                             N_("13 (amidi)")
#define STR_MID_MINOR_14                             N_("14 (admmidi)")
#define STR_MID_TEXT_SYNTH_RUNNING                   _("The synth is running.")
#define STR_MID_TEXT_SYNTH_NOT_RUNNING               _("The synth is not running.")
#define STR_MID_LABEL_SOUND_MINOR                    _("Sound minor:")
#define STR_MID_LABEL_SOUND_MINOR_TIP                _("")
#define STR_MID_LABEL_MINOR_NOTE                     _("(/dev/vmidi)")
#define STR_MID_LABEL_MINOR_NOTE_TIP                 _("")
#define STR_MID_TEXT_PICK_ANOTHER                    _("pick another")


#define STR_MID_LABEL_NO_SOUNDFONTS_FOUND_SYNTH      _("No SoundFonts found.\n\nThe synth needs one to play anything. Put a .sf2 file in any of the folders below - ~/.vlhe/sf2 needs no root - and it will appear here, or press Rescan.")
#define STR_MID_LABEL_NO_SOUNDFONTS_FOUND_SYNTH_TIP  _("")
#define STR_MID_LABEL_MOST_MUSIC_NEEDS_ONLY          _("Most music needs only a General MIDI font. A song font is for AWE32-era music that asks for its own instruments - the MIDI file switches to bank 1 when it wants them.")
#define STR_MID_LABEL_MOST_MUSIC_NEEDS_ONLY_TIP      _("")
#define STR_MID_LABEL_NONE_YET_USE_ADD               _("(none yet - use Add Folder)")
#define STR_MID_LABEL_NONE_YET_USE_ADD_TIP           _("")
#define STR_MID_LABEL_GENTLER_CURVE_WHAT_OPL3        _("The gentler curve is what the OPL3 and the original AWE32 drivers played - soft notes lose half as many decibels as the SoundFont standard takes. The right setting for DOS-era games, which were balanced against those cards.")
#define STR_MID_LABEL_GENTLER_CURVE_WHAT_OPL3_TIP    _("")
#define STR_MID_LABEL_LOWERING_RATE_FIRST_THING      _("Lowering the rate is the first thing to try when the synth cannot keep up - it buys more time per block than turning effects off does.")
#define STR_MID_LABEL_LOWERING_RATE_FIRST_THING_TIP  _("")
#define STR_MID_LABEL_TURNING_EFFECTS_OFF_ESCAPE     _("Turning the reverb off saves most of the effects' cost on a machine without the headroom for them; the chorus is cheap and can stay on.")
#define STR_MID_LABEL_TURNING_EFFECTS_OFF_ESCAPE_TIP _("")
#define STR_MID_LABEL_11_DEFAULT_THESE_SIX           _("11 is the default. These six are the device numbers no sound driver on this system uses; the others belong to the sound core, to the card, or to a driver that may be fitted later.")
#define STR_MID_LABEL_11_DEFAULT_THESE_SIX_TIP       _("")
#define STR_MID_LABEL_IF_MIDI_STOPS_RESPONDING       _("If MIDI stops responding, restarting the synth reclaims its channel without unloading the module - which would take the whole audio path down with it.")
#define STR_MID_LABEL_IF_MIDI_STOPS_RESPONDING_TIP   _("")
#define STR_MID_LABEL_SOUND_FONTS                    _("Sound Fonts")
#define STR_MID_LABEL_SOUND_FONTS_TIP                _("")
#define STR_MID_LABEL_DRIVER                         _("Driver")
#define STR_MID_LABEL_DRIVER_TIP                     _("")
#define STR_MID_FRAME_SOUNDFONTS                     _("SoundFonts")
#define STR_MID_FRAME_SOUNDFONTS_TIP                 _("")
#define STR_MID_FRAME_ALWAYS_SEARCHED                _("Always Searched")
#define STR_MID_FRAME_ALWAYS_SEARCHED_TIP            _("")
#define STR_MID_FRAME_ALSO_SEARCHED                  _("Also Searched")
#define STR_MID_FRAME_ALSO_SEARCHED_TIP              _("")
#define STR_MID_FRAME_SYNTHESIS                      _("Synthesis")
#define STR_MID_FRAME_SYNTHESIS_TIP                  _("")
#define STR_MID_FRAME_SYNTH_DAEMON                   _("Synth Daemon")
#define STR_MID_FRAME_SYNTH_DAEMON_TIP               _("")
#define STR_MID_BTN_ADD_FOLDER                       _("Add Folder...")
#define STR_MID_BTN_ADD_FOLDER_TIP                   _("")
#define STR_MID_BTN_REMOVE_FOLDER                    _("Remove Folder")
#define STR_MID_BTN_REMOVE_FOLDER_TIP                _("")
#define STR_MID_BTN_RESCAN                           _("Rescan")
#define STR_MID_BTN_RESCAN_TIP                       _("")
#define STR_MID_BTN_RESTART_SYNTH                    _("Restart Synth")
#define STR_MID_BTN_RESTART_SYNTH_TIP                _("")
#define STR_MID_CHECK_AUTO_VOICES                    _("Lower voices automatically")
#define STR_MID_CHECK_AUTO_VOICES_TIP                _("Uses fewer voices while the synth cannot keep up, and more again when it can - never more than the maximum above. Signalling the synth by hand turns this off until it restarts.")
#define STR_MID_CHECK_REVERB                         _("Reverb")
#define STR_MID_CHECK_REVERB_TIP                     _("The room around the instruments. Nearly all of the effects' cost - turn it off first on a slow machine.")
#define STR_MID_CHECK_CHORUS                         _("Chorus")
#define STR_MID_CHECK_CHORUS_TIP                     _("A slight doubling that widens the sound. Cheap - it can stay on with the reverb off.")
#define STR_MID_LABEL_MODENV                         _("Modulation envelope:")
#define STR_MID_MENU_MODENV_FAST                     _("Fast")
#define STR_MID_MENU_MODENV_SPEC                     _("To the SoundFont spec")
#define STR_MID_MENU_MODENV_TIP                      _("Fast skips a voice's pitch and filter envelope while it moves nothing and catches it up exactly the moment it starts to, so the synth does less work - worth it on a slow machine. To the SoundFont spec works every envelope out every sample. Fast rendered everything it was tested on identically to the spec mode.")
#define STR_MID_MENU_NONE                            _("(none)")
#define STR_MID_MENU_SOUNDFONT_STANDARD_SPEC         _("SoundFont standard (spec)")
#define STR_MID_MENU_GENTLER_SOUND_BLASTER_ERA       _("Gentler - Sound Blaster era (linear)")
#define STR_MID_MENU_AWE32_AWE                       _("AWE32 (awe)")
#define STR_MID_MENU_SOUNDFONT_2_01                  _("SoundFont 2.01")
#define STR_MID_MENU_SOUNDFONT_2_04                  _("SoundFont 2.04")
#define STR_MID_MENU_NONE_FLUIDSYNTH                 _("None (fluidsynth)")
#define STR_MID_MSG_PATH_TOO_LONG                    _("that path is too long")
#define STR_MID_MSG_PATH_DOES_NOT_EXIST              _("that path does not exist")
#define STR_MID_MSG_CANNOT_TELL_WHICH_FOLDER         _("cannot tell which folder that is")
#define STR_MID_MSG_WHOLE_FILESYSTEM_PICK_FOLDER     _("that is the whole filesystem - pick a folder inside it")
#define STR_MID_MSG_FOLDER_ALREADY_SEARCHED          _("that folder is already searched")
#define STR_MID_MSG_COULD_NOT_ADD_FOLDER             _("could not add that folder - the list is full")
#define STR_MID_MSG_SELECT_FOLDER_REMOVE             _("Select a folder to remove")
#define STR_MID_MSG_FONT_NOW_FOUND_DIFFERENT         _("A font is now found at a different path - Apply to keep the new location")
#define STR_MID_MSG_FOUND_MORE_SOUNDFONTS            _("Found more SoundFonts")
#define STR_MID_MSG_SOME_SOUNDFONTS_ARE_GONE         _("Some SoundFonts are gone")
#define STR_MID_MSG_COULD_NOT_RESTART_SYNTH          _("Could not restart the synth")
#define STR_MID_MSG_SYNTH_RESTARTED                  _("Synth restarted")
#define FMT_MID_FROM_CONFIGURATION                   _("%d (from the configuration)")
#define FMT_MID_VMIDI_NOT_LOADED_APPLIES             _("vmidi is not loaded - this applies when it is.")
#define FMT_MID_MODULE_RUNNING_MINOR_RELOAD          _("The module is running on minor %d. Reload vmidi to apply.")
#define FMT_MID_MIDI_SETTINGS_SOUND_MINOR            _("Midi Settings: the sound minor was refused - %.150s")


/* ==================================================================
 * CD SETTINGS - vlhe_mod_cd.c. Tabs: Drive, Options.
 * ================================================================== */

#define FMT_CD_COUNT_AUDIO                           _(", %d audio")
#define FMT_CD_COUNT_SESSIONS                        _(", %d sessions")
#define STR_CD_TEXT_PLURAL_S                         _("s")
#define STR_CD_RADIO_CDROM                           _("/dev/cdrom")
#define STR_CD_RADIO_CDROM_TIP                       _("/dev/cdrom reaches this drive - what KsCD and other CD players open. Moved at once; a player already playing keeps its disc until it reopens the drive (KsCD: Eject, then Eject again)")
#define FMT_CD_MSG_CDROM_DRIVE                       _("/dev/cdrom now reaches %s - a player already open keeps the old drive until it reopens it")
#define STR_CD_MSG_CDROM_NOT_MOVED                   _("Could not move /dev/cdrom - the disc server did not answer")
#define STR_CD_RADIO_CDG_DRIVE                       _("CD+G viewer")
#define STR_CD_RADIO_CDG_DRIVE_TIP                   _("The CD+G viewer shows and plays this drive. If it has no disc with audio, the viewer uses the first drive that does. File > Save Configuration remembers it")
#define FMT_CD_MSG_CDG_DRIVE                         _("The CD+G viewer now uses %s - File > Save Configuration keeps it")
#define STR_CD_MSG_LOADED_AT_STARTUP                 _("This disc will be loaded at startup")
#define STR_CD_MSG_NOT_LOADED_AT_STARTUP             _("This disc will not be loaded at startup")
#define STR_CD_TITLE_SELECT_DISC_IMAGE               _("Select a Disc Image")
#define STR_CD_FILTER_IMAGES                         N_("Disc images (*.cue, *.ccd, *.iso)")
#define STR_CD_FILTER_CUE                            N_("Cue sheets (*.cue)")
#define STR_CD_FILTER_CCD                            N_("CloneCD (*.ccd)")
#define STR_CD_FILTER_ISO                            N_("ISO images (*.iso)")
#define STR_CD_FILTER_ALL                            N_("All files (*)")
#define STR_CD_TAB_OPTIONS_DIRTY                     _("Options *")
#define STR_CD_TAB_OPTIONS                           _("Options")


#define STR_CD_CHECK_POINT_DEV_CDROM                 _("Point /dev/cdrom at the virtual drive")
#define STR_CD_CHECK_POINT_DEV_CDROM_TIP             _("")

#define STR_CD_LABEL_RECENTLY_USED                   _("Recently used:")
#define STR_CD_LABEL_RECENTLY_USED_TIP               _("")
#define STR_CD_LABEL_LOCKING_DRIVE_STOPS_PROGRAM_TIP _("")
#define STR_CD_LABEL_VDISC_MODULE_NOT_LOADED         _("The vdisc module is not loaded.\n\nNo virtual drives are available.")
#define STR_CD_LABEL_VDISC_MODULE_NOT_LOADED_TIP     _("")
#define STR_CD_LABEL_VDISC_MODULE_LOADED_BUT         _("The vdisc module is loaded, but vdiscd is not running.\n\nDrives exist, but nothing can open a disc image until the\ndaemon is started. Status shows what is up.")
#define STR_CD_LABEL_VDISC_MODULE_LOADED_BUT_TIP     _("")
#define STR_CD_LABEL_VIRTUAL_DRIVES                  _("Virtual drives:")
#define STR_CD_LABEL_VIRTUAL_DRIVES_TIP              _("")
#define STR_CD_LABEL_DEV_VDISC0_UPWARDS              _("(/dev/vdisc0 upwards)")
#define STR_CD_LABEL_DEV_VDISC0_UPWARDS_TIP          _("")
#define STR_CD_LABEL_PRESENT_DRIVE_AS                _("Present drive as:")
#define STR_CD_LABEL_PRESENT_DRIVE_AS_TIP            _("")
#define STR_CD_LABEL_CDG_VIEWER                      _("CD+G viewer:")
#define STR_CD_CHECK_CDG_FOLLOW_ANY                  _("Follows any disc")
#define STR_CD_CHECK_CDG_FOLLOW_ANY_TIP              _("When the drive chosen on the Drive tab has no disc with audio, the viewer uses the first drive that does. Off: it stays on the chosen drive and shows no disc")
#define STR_CD_CHECK_CDG_STOP_ON_SWAP                _("Stops the old disc on a change")
#define STR_CD_CHECK_CDG_STOP_ON_SWAP_TIP            _("When the viewer moves to another drive, stop the disc it was playing. Off: that disc plays on. A disc started from KsCD is never stopped")
#define STR_CD_LABEL_60_63_KERNEL_S                  _("60-63 is the kernel's range for devices with no official number.")
#define STR_CD_LABEL_60_63_KERNEL_S_TIP              _("")
#define STR_CD_LABEL_OUR_KNOWLEDGE_ONLY_CDPARANOIA   _("To our knowledge only cdparanoia needs this. cdda2wav, KsCD and grip all work without it.\n\nUse at your own risk: these numbers belong to real drivers. Claiming one can stop that hardware working, or stop this module loading if the real driver gets there first - including a drive you install later.")
#define STR_CD_LABEL_OUR_KNOWLEDGE_ONLY_CDPARANOIA_TIP _("")
#define STR_CD_LABEL_IMPERSONATE_AS                  _("Impersonate as:")
#define STR_CD_LABEL_IMPERSONATE_AS_TIP              _("")
#define STR_CD_LABEL_NEEDED_RIP_AUDIO_CDPARANOIA     _("Needed to rip audio with cdparanoia or cdda2wav, and to play Video CDs. Off means those tools cannot see the drive.")
#define STR_CD_LABEL_NO_PACKET_INTERFACE             _("Not available: this kernel has no CD-ROM packet interface (it arrived in Linux 2.2.16), so ripping with cdparanoia or cdda2wav and playing Video CDs cannot work. Discs still mount and play.")
#define STR_CD_LABEL_NEEDED_RIP_AUDIO_CDPARANOIA_TIP _("")
#define STR_CD_LABEL_CD_PLAYERS_RIPPERS_LOOK         _("CD players and rippers look for /dev/cdrom. What it pointed at is saved and put back when you unload.")
#define STR_CD_LABEL_CD_PLAYERS_RIPPERS_LOOK_TIP     _("")
#define STR_CD_LABEL_DRIVE                           _("Drive")
#define STR_CD_LABEL_DRIVE_TIP                       _("")
#define STR_CD_BTN_KEEP                              _("Keep")
#define STR_CD_BTN_KEEP_TIP                          _("")
#define STR_CD_BTN_REMOVE                            _("Remove")
#define STR_CD_BTN_REMOVE_TIP                        _("")
#define STR_CD_BTN_EJECT                             _("Eject")
#define STR_CD_BTN_EJECT_TIP                         _("")
#define STR_CD_BTN_IMAGE                             _("Image...")
#define STR_CD_BTN_IMAGE_TIP                         _("")
#define STR_CD_CHECK_LOAD_STARTUP                    _("Load at startup")
#define STR_CD_CHECK_LOAD_STARTUP_TIP                _("Put this drive's disc back at the next load - at boot, or when a portable copy is loaded. Belongs to the drive: a new disc in it comes back too. Needs \"Reattach the drives marked Load at startup\" on the Options tab (on by default)")
#define STR_CD_CHECK_IMPERSONATE_PERIOD_CD_ROM       _("Impersonate a period CD-ROM drive (for cdparanoia)")
#define STR_CD_CHECK_IMPERSONATE_PERIOD_CD_ROM_TIP   _("")
#define STR_CD_CHECK_ANSWER_MMC_PACKET_COMMANDS      _("Answer MMC packet commands")
#define STR_CD_CHECK_ANSWER_MMC_PACKET_COMMANDS_TIP  _("")
#define STR_CD_CHECK_REATTACH_DRIVES                 _("Reattach the drives marked Load at startup")
#define STR_CD_CHECK_REATTACH_DRIVES_TIP             _("When the disc server starts - at boot or at Load - put back the disc of every drive whose Load at startup box is ticked. Off: every drive starts empty, whatever the boxes say.")
#define STR_CD_MENU_NO_RECENT_IMAGES                 _("(no recent images)")
#define STR_CD_MENU_RECENT_IMAGES                    _("Recent images...")
#define STR_CD_TITLE_IMAGE_NOT_AVAILABLE             _("Image Not Available")
#define STR_CD_TEXT_NO_DISC                          _("(no disc)")
#define STR_CD_MSG_COULD_NOT_CHANGE_DRIVE            _("Could not change that - the disc server did not record it (is it running?)")
#define STR_CD_MSG_COULD_NOT_EJECT_DRIVE             _("Could not eject - the drive is in use")
#define STR_CD_MSG_DISC_EJECTED                      _("Disc ejected")
#define STR_CD_MSG_REMOVED_FROM_RECENT_LIST          _("Removed from the recent list")
#define STR_CD_MSG_KEPT_RECENT_LIST                  _("Kept in the recent list")
#define STR_CD_MSG_DISC_ATTACHED                     _("Disc attached")
#define STR_CD_MSG_COULD_NOT_ATTACH_IMAGE            _("Could not attach - the image is there, so check that vdiscd is running (see Status)")
#define STR_CD_MSG_COULD_NOT_ATTACH_DENIED           _("Could not attach - the disc server may not read this file. Installed, it runs as the vlhe account: keep images outside /root, readable by other users")
#define STR_CD_MSG_COULD_NOT_ATTACH_REFUSED          _("Could not attach - the disc server could not read it as a disc image, or the drive is in use")
#define FMT_CD_TRACK                                 _("%s - %d track%s")
#define FMT_CD_TRACK_2                               _("%d track%s")
#define FMT_CD_DISC_IMAGE_COULD_NOT                  _("This disc image could not be opened:\n\n    %s\n\nIt may have been moved or deleted, or it may be on a\nvolume that is not available right now.\n\nRemove it from the recent list?")
#define FMT_CD_USE                                   _("%d - %.60s (in use)")
#define FMT_CD_MAJOR_ALREADY_USE_PICK                _("That major is already in use - pick another.")
#define FMT_CD_VDISC_NOT_LOADED_THESE                _("vdisc is not loaded - these apply when it is.")
#define FMT_CD_MODULE_RUNNING_DRIVE_RELOAD           _("The module is running with %d drive%s. Reload vdisc to apply.")
#define FMT_CD_NOT_APPLIED_YET                       _("Not applied yet.")
#define FMT_CD_THESE_MATCH_RUNNING_MODULE            _("These match the running module.")
#define FMT_CD_CD_SETTINGS_REFUSED_MAJOR             _("CD Settings: refused - major %d must be one the menus offer and drives %d must be 1 to %d")


/* ==================================================================
 * CD+G VIEWER - vlhe_mod_cdg.c. The transport, the track list, the
 * information line; then its Options dialog.
 * ================================================================== */

#define STR_CDG_FIELD_TITLE                          N_("Title")
#define STR_CDG_FIELD_PERFORMER                      N_("Performer")
#define STR_CDG_FIELD_SONGWRITER                     N_("Songwriter")
#define STR_CDG_FIELD_COMPOSER                       N_("Composer")
#define STR_CDG_FIELD_ARRANGER                       N_("Arranger")
#define STR_CDG_FIELD_MESSAGE                        N_("Message")
#define STR_CDG_FIELD_TRACK_NUMBER                   N_("Track number")
#define STR_CDG_FIELD_TIME                           N_("Time")
#define STR_CDG_MSG_OPTIONS_APPLIED                  _("Viewer options applied")
#define STR_CDG_MSG_OPTIONS_APPLIED_REFUSED          _("Viewer options applied - could not be kept (a value was refused)")
#define STR_CDG_MSG_OPTIONS_APPLIED_SAVE             _("Viewer options applied - File > Save Configuration keeps them")
#define STR_CDG_LABEL_AVAILABLE                      _("Available")
#define STR_CDG_LABEL_AVAILABLE_TIP                  _("")
#define STR_CDG_LABEL_SHOWN_IN_ORDER                 _("Shown, in order")
#define STR_CDG_LABEL_SHOWN_IN_ORDER_TIP             _("")
#define STR_CDG_TEXT_NO_DISC_DAEMON                  _("no disc, or the daemon did not answer")
#define STR_CDG_MSG_NEAR_END_OF_TRACK                _("Already near the end of the track")
#define STR_CDG_MSG_COULD_NOT_SEEK                   _("Could not seek")
#define STR_CDG_BTN_SHOW                             _("Show")
#define STR_CDG_BTN_SHOW_TIP                         _("")
#define STR_CDG_MSG_GRAPHICS_HIDDEN                  _("Graphics hidden - player only")
#define STR_CDG_MSG_GRAPHICS_SHOWN                   _("Graphics shown")
#define STR_CDG_MSG_VIEWER_DETACHED                  _("Viewer detached")
#define STR_CDG_MSG_VIEWER_REATTACHED                _("Viewer re-attached")
#define STR_CDG_TEXT_NO_DISC_LIST                    _("(no disc)")
#define FMT_CDG_TRACK_UNKNOWN                        _("%02d: <Unknown>")
#define FMT_CDG_TRACK_N                              _("Track %d")
#define STR_CDG_TEXT_NO_DISC_INFORMATION             _("(no disc information)")


/* The transport buttons are icons with no label, so each has only
 * its tip. The previous-track button said "Start of track" until
 * 2026-10-02 (design/47 section 7 fault 2); the user: "change it". */
#define STR_CDG_BTN_PREV_TIP                         _("Previous track")
#define STR_CDG_BTN_RW_TIP                           _("Back 30 seconds")
#define STR_CDG_BTN_PLAY_TIP                         _("Play")
#define STR_CDG_BTN_STOP_TIP                         _("Stop")
#define STR_CDG_BTN_FF_TIP                           _("Forward 30 seconds")
#define STR_CDG_BTN_NEXT_TIP                         _("Next track")


#define STR_CDG_LABEL_OFF_KEEPS_LAST_FRAME           _("Off keeps the last frame on screen until the next disc paints over it, as a player's screen does.")
#define STR_CDG_LABEL_OFF_KEEPS_LAST_FRAME_TIP       _("")
#define STR_CDG_LABEL_CONTROLS_STAY_BOTTOM_ONLY      _("The controls stay at the bottom;\nonly the picture moves.\nDetached, only left/centre/right\nis used.")
#define STR_CDG_LABEL_CONTROLS_STAY_BOTTOM_ONLY_TIP  _("")
#define STR_CDG_LABEL_TEXT                           _("--:--")
#define STR_CDG_LABEL_TEXT_TIP                       _("")
#define STR_CDG_LABEL_NO_DISC_INFORMATION            _("(no disc information)")
#define STR_CDG_LABEL_NO_DISC_INFORMATION_TIP        _("")
#define STR_CDG_FRAME_PICTURE_SIZE                   _("Picture size")
#define STR_CDG_FRAME_PICTURE_SIZE_TIP               _("")
#define STR_CDG_FRAME_BORDER                         _("Border")
#define STR_CDG_FRAME_BORDER_TIP                     _("")
#define STR_CDG_FRAME_WHEN_DISC_EJECTED              _("When the disc is ejected")
#define STR_CDG_FRAME_WHEN_DISC_EJECTED_TIP          _("")
#define STR_CDG_FRAME_PICTURE_POSITION               _("Picture position")
#define STR_CDG_FRAME_PICTURE_POSITION_TIP           _("")
#define STR_CDG_FRAME_WHEN_DETACHED                  _("When detached")
#define STR_CDG_FRAME_WHEN_DETACHED_TIP              _("")
#define STR_CDG_FRAME_TRACK_LIST_SHOWS               _("Track list shows")
#define STR_CDG_FRAME_TRACK_LIST_SHOWS_TIP           _("")
#define STR_CDG_FRAME_INFORMATION_LINE               _("Information line")
#define STR_CDG_FRAME_INFORMATION_LINE_TIP           _("")
#define STR_CDG_BTN_ADD                              _("Add ->")
#define STR_CDG_BTN_ADD_TIP                          _("")
#define STR_CDG_BTN_REMOVE                           _("<- Remove")
#define STR_CDG_BTN_REMOVE_TIP                       _("")
#define STR_CDG_BTN_MOVE_UP                          _("Move up")
#define STR_CDG_BTN_MOVE_UP_TIP                      _("")
#define STR_CDG_BTN_MOVE_DOWN                        _("Move down")
#define STR_CDG_BTN_MOVE_DOWN_TIP                    _("")
#define STR_CDG_BTN_OPTIONS                          _("Options...")
#define STR_CDG_BTN_OPTIONS_TIP                      _("")
#define STR_CDG_BTN_HIDE                             _("Hide")
#define STR_CDG_BTN_HIDE_TIP                         _("Hide the graphics - a player and nothing else")
#define STR_CDG_BTN_ATTACH                           _("Attach")
#define STR_CDG_BTN_ATTACH_TIP                       _("")
#define STR_CDG_BTN_DETACH                           _("Detach")
#define STR_CDG_BTN_DETACH_TIP                       _("Float the viewer in its own window")
#define STR_CDG_CHECK_HIDE_OUTER_EDGE_AS             _("Hide the outer edge, as a CD+G player does")
#define STR_CDG_CHECK_HIDE_OUTER_EDGE_AS_TIP         _("")
#define STR_CDG_CHECK_CLEAR_PICTURE                  _("Clear the picture")
#define STR_CDG_CHECK_CLEAR_PICTURE_TIP              _("")
#define STR_CDG_CHECK_ATTACH_TRACK_LIST_SECOND       _("Attach and track list on a second row")
#define STR_CDG_CHECK_ATTACH_TRACK_LIST_SECOND_TIP   _("")
#define STR_CDG_RADIO_2X_600_X_432                   _("2x  (600 x 432)")
#define STR_CDG_RADIO_2X_600_X_432_TIP               _("")
#define STR_CDG_RADIO_1X_300_X_216                   _("1x  (300 x 216)")
#define STR_CDG_RADIO_1X_300_X_216_TIP               _("")
#define STR_CDG_RADIO_TITLE_01_SONG_JOY              _("The title            01: Song of Joy")
#define STR_CDG_RADIO_TITLE_01_SONG_JOY_TIP          _("")
#define STR_CDG_RADIO_ARTIST_TITLE_01_PURRS          _("Artist and title     01: The Purrs - Song of Joy")
#define STR_CDG_RADIO_ARTIST_TITLE_01_PURRS_TIP      _("")
#define STR_CDG_TITLE_CD_G_VIEWER_OPTIONS            _("CD+G Viewer Options")
#define STR_CDG_MSG_NO_DISC_AUDIO_ATTACH             _("No disc with audio - attach one on the CD Settings page")
#define STR_CDG_MSG_COULD_NOT_PAUSE                  _("Could not pause")
#define STR_CDG_MSG_PAUSED                           _("Paused")
#define STR_CDG_MSG_COULD_NOT_RESUME                 _("Could not resume")
#define STR_CDG_MSG_PLAYING                          _("Playing")
#define STR_CDG_MSG_COULD_NOT_STOP_DRIVE             _("Could not stop the drive")
#define STR_CDG_MSG_STOP                             _("Stop")
#define STR_CDG_MSG_NOTHING_PLAYING                  _("Nothing is playing")
#define STR_CDG_MSG_CANNOT_TELL_WHERE_TRACK          _("Cannot tell where this track starts")
#define STR_CDG_MSG_NO_DISC_PLAY                     _("No disc to play")
#define FMT_CDG_COULD_NOT_START_PLAYBACK             _("Could not start playback: %.40s")
#define FMT_CDG_PLAYING_TRACK                        _("Playing track %d")
#define FMT_CDG_SECONDS_INTO_TRACK                   _("%d seconds into track %d")
#define FMT_CDG_COULD_NOT_PLAY_TRACK                 _("Could not play track %d")
#define FMT_CDG_TRACK                                _("Track %d")
#define STR_CDG_TIP_ELAPSED_TIME_CURRENT_TRACK       _("Elapsed time in the current track. If this is moving, the disc is playing - even if you cannot hear it.")


/* ==================================================================
 * RENDER MIDI - vlhe_mod_render.c. Tabs: Render, Advanced; the
 * space question; the file picker.
 * ================================================================== */

#define STR_RND_TITLE_SELECT_MIDI_FILE               _("Select a MIDI File")
#define STR_RND_FILTER_MIDI                          N_("MIDI files (*.mid, *.midi)")
#define STR_RND_FILTER_ALL                           N_("All files (*)")
#define STR_RND_TEXT_RENDERING_WAV_IN_TMP            _("rendering, WAV in /tmp")
#define STR_RND_TEXT_RENDERING                       _("rendering")
#define STR_RND_TEXT_ENCODING                        _("encoding")
#define STR_RND_MSG_RENDERS_USE_MIDI_SETTINGS        _("Renders will use the Midi Settings")
#define STR_RND_MSG_RENDERS_USE_ADVANCED             _("Renders will use the Advanced tab")
#define STR_RND_TEXT_GM_FOLLOW                       _("(the Midi Settings' font)")
#define STR_RND_TEXT_SONG_NONE                       _("(none)")
#define STR_RND_LAW_SPEC                             N_("spec - SoundFont standard, (v/127)^2")
#define STR_RND_LAW_LINEAR                           N_("linear - gentler, Sound Blaster era")
#define STR_RND_VF_AWE                               N_("awe - the AWE32's")
#define STR_RND_VF_201                               N_("2.01 - SoundFont 2.01")
#define STR_RND_VF_204                               N_("2.04 - SoundFont 2.04")
#define STR_RND_VF_NONE                              N_("none")
#define STR_RND_LABEL_GENERAL_MIDI                   _("General MIDI:")
#define STR_RND_LABEL_GENERAL_MIDI_TIP               _("")
#define STR_RND_LABEL_SONG_FONT                      _("Song font:")
#define STR_RND_LABEL_SONG_FONT_TIP                  _("")
#define STR_RND_LABEL_VOICES                         _("Voices:")
#define STR_RND_LABEL_VOICES_TIP                     _("")
#define STR_RND_LABEL_SAMPLE_RATE                    _("Sample rate:")
#define STR_RND_LABEL_SAMPLE_RATE_TIP                _("")
#define STR_RND_LABEL_MASTER_GAIN                    _("Master gain:")
#define STR_RND_LABEL_MASTER_GAIN_TIP                _("")
#define STR_RND_LABEL_VOLUME_LAW                     _("Volume law:")
#define STR_RND_LABEL_VOLUME_LAW_TIP                 _("")
#define STR_RND_LABEL_VELOCITY_FILTER                _("Velocity filter:")
#define STR_RND_LABEL_VELOCITY_FILTER_TIP            _("")
#define STR_RND_LABEL_MIDI_FILE                      _("MIDI file:")
#define STR_RND_LABEL_MIDI_FILE_TIP                  _("")
#define STR_RND_LABEL_SAVE_AS                        _("Save as:")
#define STR_RND_LABEL_SAVE_AS_TIP                    _("")
#define STR_RND_TEXT_ONE_STACKED_ABOVE               _(" (and one stacked above it)")
#define STR_RND_LABEL_SOUNDFONT                      _("Soundfont:")
#define STR_RND_LABEL_SOUNDFONT_TIP                  _("")
#define STR_RND_LABEL_LAME_IS_AT                     _("LAME is at:")
#define STR_RND_LABEL_LAME_IS_AT_TIP                 _("")
#define STR_RND_WAVTEMP_AUTO                         N_("Automatic - whichever has more free space")
#define STR_RND_WAVTEMP_TMP                          N_("In /tmp")
#define STR_RND_WAVTEMP_BESIDE                       N_("Beside the output")
#define STR_RND_LABEL_TEMPORARY_WAV                  _("Temporary WAV:")
#define STR_RND_LABEL_TEMPORARY_WAV_TIP              _("")
#define STR_RND_LABEL_PROGRESS                       _("Progress:")
#define STR_RND_LABEL_PROGRESS_TIP                   _("")
#define STR_RND_TAB_RENDER                           N_("Render")
#define STR_RND_TAB_ADVANCED                         N_("Advanced")
#define STR_RND_TEXT_AND_SONG_FONT_FROM_ADVANCED     _(" (and a song font, from Advanced)")
#define STR_RND_TEXT_FROM_ADVANCED                   _(" (from Advanced)")
#define STR_RND_TEXT_AND_SONG_FONT_FROM_MIDI         _(" (and a song font, from Midi Settings)")
#define STR_RND_TEXT_FROM_MIDI_SETTINGS              _(" (from Midi Settings)")


#define STR_RND_LABEL_1_2048_OFFLINE_HAS             _("1 - 2048, offline has no deadline")
#define STR_RND_LABEL_1_2048_OFFLINE_HAS_TIP         _("")
#define STR_RND_LABEL_HZ                             _("Hz")
#define STR_RND_LABEL_HZ_TIP                         _("")
#define STR_RND_LABEL_1_0_UNITY_2                    _("1.0 unity, 2.0 max - it clips")
#define STR_RND_LABEL_1_0_UNITY_2_TIP                _("")
#define STR_RND_LABEL_THESE_ARE_SETTINGS_RENDERING   _("These are the settings for rendering files only. These do not change the Midi Settings.\n\nRendering with more voices increases render time.")
#define STR_RND_LABEL_THESE_ARE_SETTINGS_RENDERING_TIP _("")
#define STR_RND_LABEL_NONE_SET_CHOOSE_ONE            _("none set - choose one in Midi Settings and press Apply")
#define STR_RND_LABEL_NONE_SET_CHOOSE_ONE_TIP        _("")
#define STR_RND_LABEL_RENDERING_OPENS_NO_SOUND       _("Rendering opens no sound device, so it works while something else is playing, and on a machine too slow to play the file in real time. The result is what the synth would have sounded like, from the same code.\n\nMP3 needs LAME, which Corel does not ship - name the binary above if you have one.")
#define STR_RND_LABEL_RENDERING_OPENS_NO_SOUND_TIP   _("")
#define STR_RND_LABEL_RENDER                         _("Render")
#define STR_RND_LABEL_RENDER_TIP                     _("")
#define STR_RND_LABEL_ADVANCED                       _("Advanced")
#define STR_RND_LABEL_ADVANCED_TIP                   _("")
#define STR_RND_FRAME_RENDER_MIDI_FILE               _("Render a MIDI file")
#define STR_RND_FRAME_RENDER_MIDI_FILE_TIP           _("")
#define STR_RND_BTN_RENDER_ANYWAY                    _("Render anyway")
#define STR_RND_TITLE_LONG_RENDER                    _("A long render")
#define FMT_RND_LONG_RENDER                          _("This MIDI file is about %lu:%02lu long, and the render will take about %lu MB. Render it?")
#define FMT_RND_LONG_RENDER_CAPPED                   _("This MIDI file claims to be longer than VLHE will render in one file. The render will stop at about %lu:%02lu, about %lu MB. Render it anyway?")
#define STR_RND_BTN_RENDER_IT                        _("Render")
#define STR_RND_BTN_RENDER_IT_TIP                    _("Render the whole file.")
#define STR_RND_BTN_RENDER_ANYWAY_TIP                _("")
#define STR_RND_BTN_BROWSE                           _("Browse...")
#define STR_RND_BTN_BROWSE_TIP                       _("")
#define STR_RND_CHECK_LOW_PASS_FILTER_SPEEDS         _("Disable Low Pass Filter - speeds up rendering, difference should not be audible")
#define STR_RND_CHECK_LOW_PASS_FILTER_SPEEDS_TIP     _("")
#define STR_RND_LABEL_MODENV                         _("Modulation envelope:")
#define STR_RND_MENU_MODENV_TIP                      _("Fast skips a voice's pitch and filter envelope while it moves nothing and catches it up exactly when it starts to, so the render finishes sooner. To the SoundFont spec works every envelope out every sample. Fast rendered everything it was tested on identically to the spec mode.")
#define STR_RND_CHECK_USE_VLHE_SETTINGS_RENDER       _("Use VLHE settings - render it the way the machine plays it")
#define STR_RND_CHECK_USE_VLHE_SETTINGS_RENDER_TIP   _("")
#define STR_RND_CHECK_ENCODE_MP3_AFTERWARDS_LAME     _("Encode to MP3 afterwards, with LAME")
#define STR_RND_CHECK_ENCODE_MP3_AFTERWARDS_LAME_TIP _("")
#define STR_RND_MENU_NO_SOUNDFONTS_FOUND             _("(no SoundFonts found)")
#define STR_RND_TITLE_NOT_ENOUGH_SPACE               _("Not enough space?")
#define STR_RND_TITLE_LAME_FAILED                    _("LAME failed")
#define STR_RND_MSG_DIRECTORY_PICK_MID_FILE          _("that is a directory - pick a .mid file")
#define STR_RND_MSG_RENDER_ALREADY_RUNNING           _("a render is already running")
#define STR_RND_MSG_CANCELLING                       _("cancelling...")
#define STR_RND_MSG_PICK_MIDI_FILE_FIRST             _("pick a MIDI file first")
#define STR_RND_MSG_GIVE_OUTPUT_NAME                 _("give the output a name")
#define STR_RND_MSG_NO_SOUNDFONT_SET_CHOOSE          _("no soundfont is set - choose one in Midi Settings and press Apply")
#define STR_RND_MSG_SONG_FONT_NO_GM                  _("only a song font is set - choose a General MIDI font in Midi Settings and press Apply")
#define STR_RND_MSG_SAY_WHERE_LAME_TURN              _("say where LAME is, or turn MP3 off")
#define STR_RND_MSG_LAME_PATH_NOT_EXECUTABLE         _("that LAME path is not an executable file")
#define STR_RND_MSG_RENDERING                        _("rendering...")
#define STR_RND_MSG_NO_GENERAL_MIDI_FONT             _("no General MIDI font - choose one in Midi Settings, or pick one on the Advanced tab")
#define STR_RND_MSG_CANCELLED                        _("cancelled")
#define STR_RND_MSG_CANCELLED_PARTIAL_FILE_WAS       _("cancelled - the partial file was removed")
#define STR_RND_MSG_SMF2WAV_NOT_PATH                 _("smf2wav is not on the PATH")
#define STR_RND_MSG_STOPPED_20_MINUTE_CEILING        _("stopped at the most VLHE renders in one file - the file is incomplete")
#define STR_RND_MSG_RENDER_FAILED_SAID_NOTHING       _("the render failed, and said nothing about why")
#define STR_RND_MSG_DONE                             _("done")
#define STR_RND_MSG_ENCODING                         _("encoding...")
#define STR_RND_MSG_LAME_FAILED_NOT_PATH             _("LAME failed - it is not at that path")
#define STR_RND_MSG_LAME_FAILED_CHECK_PATH           _("LAME failed - check the path and the file")
#define STR_RND_MSG_COULD_NOT_SAVE_RENDER            _("could not save the render settings")
#define STR_RND_TEXT_CAPTURE_COULD_NOT_READ          _("(the capture could not be read back)\n")
#define FMT_RND_U_U_MB_APPROX                        _("%.30s... %d%%  -  %lu.%lu MB of approx %lu MB")
#define FMT_RND_U_U_FRAMES_ABOUT                     _("%.20s... %d%%  -  %lu of %lu frames, about %d:%02d left")
#define FMT_RND_U_U_FRAMES                           _("%.20s... %d%%  -  %lu of %lu frames")
#define FMT_RND_RENDER_ESTIMATED_ABOUT_U             _("This render is estimated at about %lu MB,\nand the disk has %lu MB free.\n\nThe estimate is approximate - it depends on the\nsample rate and on how long the last notes take\nto fade, which is not known until they have.\n\nRendering anyway may produce an incomplete file.")
#define FMT_RND_RENDER_FAILED                        _("the render failed - %.180s")
#define FMT_RND_LAME_FAILED_EXIT_OUTPUT              _("LAME failed (exit %d) - its output is in %.60s")
#define FMT_RND_LAME_EXITED_STATUS_OUTPUT            _("\nLAME exited with status %d.  This output is kept in\n%.80s\n")


/* ==================================================================
 * THE CLI - vlhe_cli.c and vlhe_apply.c's shell output, where it is
 * read by a person: the simulation and result text, the refusals.
 * Last, because the user asked for the GUI; the CLI shares most of
 * these through vlhe_apply.c and takes what it shares.
 * ================================================================== */

#endif /* VLHE_STRINGS_H */
