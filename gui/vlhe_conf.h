/*
 * vlhe_conf.h - the config files, read and written.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * THE FORMAT IS design/09's, SETTLED 2026-09-11: duke3d.cfg's sections
 * and explanatory comments, boom.cfg's named values. design/33 section
 * 4 has the citation and design/vsound.conf.example is the worked
 * example every key here is taken from.
 *
 *     ; a comment
 *     [Sound Settings]
 *     MidiChannel = 1
 *
 * THREE FILES, NOT ONE - design/33 section 1. design/09 said one file;
 * that settled the FORMAT and the CONTENT, which are unchanged here,
 * and did not consider PERMISSION. A single root-owned file would make
 * swapping a disc a root operation, so:
 *
 *   /etc/vlhe.conf          machine settings   root, rarely written
 *   /var/lib/vlhe/state/drives  what is attached   group, written often
 *   ~/.vlhe/vlhe.conf       per-person         that user
 *
 * WE OWN THESE FILES AND GENERATE THEM. Every key is written with its
 * explanatory comment above it, the way vsound.conf.example has them,
 * so the file teaches its own format. That is the conf.modules route -
 * see vlhe_conf_write()'s comment for what it costs.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_CONF_H
#define VLHE_CONF_H

#include <stdio.h>      /* FILE, for the body callback below */

#define VLHE_CONF_KEY_MAX    64
#define VLHE_CONF_VAL_MAX   256
#define VLHE_CONF_SEC_MAX    64

/* A parsed file. Flat on purpose: these files hold tens of keys, not
 * thousands, and a list beats a hash table nobody can read. The
 * section is stored PER ENTRY rather than as a tree - lookup is
 * "[Sound Settings] MidiChannel", which is how every caller asks. */
#define VLHE_CONF_MAX_ENTRIES 128

struct vlhe_conf_entry {
    char section[VLHE_CONF_SEC_MAX];
    char key[VLHE_CONF_KEY_MAX];
    char value[VLHE_CONF_VAL_MAX];
};

struct vlhe_conf {
    struct vlhe_conf_entry entry[VLHE_CONF_MAX_ENTRIES];
    int n;
    /*
     * UNSAVED CHANGES. Set by set/set_int/unset when a value actually
     * CHANGES, cleared by read and by a successful write.
     *
     * IT PROTECTS THE BACKUP RATHER THAN SAVING A WRITE. design/33
     * keeps one `.old' per file, so writing when nothing changed
     * copies the live file over its own backup and destroys the
     * previous version - two idle clicks on OK and it is gone.
     *
     * SET ONLY ON A REAL CHANGE, which is why the setters compare
     * first: setting a value to what it already holds must leave the
     * file alone. Same discipline as vsound_chan_setmute() bumping
     * the generation only when the flag moves.
     */
    int dirty;
};

/* Read a file. A MISSING FILE IS NOT AN ERROR - it yields an empty
 * conf and 0, because every key has a default and a machine with no
 * config file must still work. Returns -1 only when the file exists
 * and could not be read.
 *
 * MALFORMED LINES ARE SKIPPED, NOT FATAL. A hand-edited file with one
 * bad line loses that line and keeps the rest, which is what a user
 * wants; refusing the whole file would lose settings that parsed. */
/* Copy a file. Returns 0, or -1 with errno set; a missing source is
 * success with nothing done. Public because the .old restore needs
 * the same copy the .old backup uses. */
int vlhe_conf_copy(const char *from, const char *to);

int vlhe_conf_read(struct vlhe_conf *c, const char *path);

/*
 * READ A FILE ROOT WILL ACT ON, AND SAY WHETHER IT MAY - design/49 T0.
 *
 * Reads exactly as vlhe_conf_read() does, so the pages still show
 * what is in the file, and ALSO judges the file. Returns 0 when it is
 * trustworthy (or missing - the defaults are compiled in), 1 when it
 * was read but must not be acted on, with the reason in `why', and -1
 * as vlhe_conf_read() does.
 *
 * TRUSTWORTHY MEANS ONLY ROOT CAN HAVE WRITTEN IT: a regular file,
 * not a link, owned by root, not writable by others (by its group
 * only if that group is root), in a directory owned by root that
 * others cannot write unless it is sticky. The file is judged by
 * fstat() on the descriptor that is then parsed, so it cannot be
 * swapped between the check and the read.
 */
int vlhe_conf_read_trusted(struct vlhe_conf *c, const char *path,
                           char *why, int max);

/* Read a file someone else handed us: a REGULAR file only, opened so
 * the open cannot block. Returns 0, or -1 with errno (EINVAL: not a
 * regular file). Nothing read is trusted - the caller checks values. */
int vlhe_conf_read_regular(struct vlhe_conf *c, const char *path);

/* Look a key up. Returns the value, or `dflt' when absent - so a
 * caller never has to test for existence separately. */
/*
 * IS VLHE INSTALLED ON THIS MACHINE?
 *
 * `[Paths] ModuleDir' in /etc/vlhe.conf, which the installer writes -
 * a declaration rather than a probe, for the reason that key's
 * comment in vlhe_conf_template.c gives.
 *
 * SEPARATE FROM "AM I A PORTABLE COPY", which vlhe_self_is_trial()
 * answers from a marker beside the binary. Both can be true at once,
 * and that combination is the interesting one: a user with VLHE
 * installed who unpacks a newer build to test it.
 */
int vlhe_conf_machine_installed(void);

const char *vlhe_conf_get(const struct vlhe_conf *c,
                          const char *section, const char *key,
                          const char *dflt);

/* The same, parsed. A value that is not a number yields `dflt', on the
 * same reasoning as a missing one: the default is always right enough
 * to draw a window with. */
int vlhe_conf_get_int(const struct vlhe_conf *c,
                      const char *section, const char *key, int dflt);

/* Set a key, replacing it if present and appending if not. Returns 0,
 * or -1 if the table is full. */
int vlhe_conf_set(struct vlhe_conf *c,
                  const char *section, const char *key, const char *value);
int vlhe_conf_set_int(struct vlhe_conf *c,
                      const char *section, const char *key, int value);

/* Remove a key. Returns 0 if it was there. */
int vlhe_conf_unset(struct vlhe_conf *c,
                    const char *section, const char *key);

/* WRITE THE FILE, SAFELY - design/33 section 3, and the order is the
 * whole point:
 *
 *   1. write <path>.tmp
 *   2. fsync it, then close        <- the data reaches the disk BEFORE
 *   3. copy <path> -> <path>.old      anything points at it
 *   4. rename(.tmp, path)          <- atomic
 *
 * STEP 3 IS A COPY, NOT A MOVE. The obvious rename(path, path.old)
 * then rename(tmp, path) leaves a window where the file DOES NOT
 * EXIST, and a halt there leaves a booting machine with no config.
 *
 * AND THE fsync IS NOT OPTIONAL. rename() is atomic in the DIRECTORY,
 * but on ext2 the new file's data blocks may not be on disk yet - a
 * power loss just after the rename can leave a valid inode full of
 * NULLS, which is worse than a missing file because it parses as empty
 * and the "use defaults" path never fires. 2.2 has no data=ordered.
 *
 * `backup' is 0 for the files rewritten constantly - the drive state
 * and the per-user file - where a .old per disc swap is noise.
 *
 * `header' is written at the top as a comment block, or NULL. Returns
 * 0, or -1 with errno set. */
int vlhe_conf_write(const struct vlhe_conf *c, const char *path,
                    const char *header, int backup);

/* THE SAME SEQUENCE, FOR A BODY SOMEBODY ELSE FORMATTED.
 * vlhe_conf_template_write() lays out its own text - keys in template
 * order with their comments above them - and needs the identical
 * fsync-copy-rename to put it down. Sharing this rather than copying
 * the sequence is the point: two implementations of a crash-safe
 * write would drift, and only one of them would be tested.
 *
 * THE BODY IS WRITTEN BY A CALLBACK, NOT PASSED AS A STRING, because
 * the first version passed a string built in a fixed buffer and
 * SILENTLY TRUNCATED at 1 KB - a valid-looking file missing 22 of its
 * 23 keys. A callback writing straight to the stream has no size to
 * overrun.
 *
 * `fn' returns 0, or -1 to abandon the write (the .tmp is removed and
 * the original is untouched). */
typedef int (*vlhe_conf_body_fn)(FILE *fp, const struct vlhe_conf *c,
                                 int arg);

int vlhe_conf_write_fn(const char *path, const char *header,
                       vlhe_conf_body_fn fn, const struct vlhe_conf *c,
                       int arg, int backup);

/* WHERE THE FILES ARE. Returned as static buffers - the caller does
 * not free and does not keep them across another call.
 *
 * The user path expands $HOME, and returns NULL when there is none,
 * which a daemon started by init genuinely has. */
const char *vlhe_conf_system_path(void);   /* /etc/vlhe.conf          */
const char *vlhe_conf_drives_path(void);   /* /var/lib/vlhe/state/drives, or
                                            * `drives' in a portable
                                            * folder                  */
const char *vlhe_conf_user_path(void);     /* see the tiers below     */

/*
 * THE DAEMONS' ACCOUNT - design/33 section 3k, the rulings of
 * 2026-09-30. An installed VLHE runs its daemons as `vlhe', a system
 * account nobody logs in as; the nodes, the FIFOs and /var/lib/vlhe
 * are its group's. READ BY HAND from /etc/passwd and /etc/group, as
 * uid_name() reads names, because static glibc 2.1's getpwnam()
 * dlopen()s NSS. VLHE_PASSWD and VLHE_GROUP redirect the two files,
 * for the host tests only.
 *
 * `groups' is the account's primary group first, then every group
 * whose member list names it - the supplementary set the daemons get,
 * so the installer's `adduser vlhe audio' is what lets the pump open
 * a card node that is root:audio 0660, as stock Corel's are.
 */
#define VLHE_ACCOUNT        "vlhe"
#define VLHE_ACCOUNT_GROUPS 16

struct vlhe_account {
    long uid;
    long gid;
    int  ngroups;
    long groups[VLHE_ACCOUNT_GROUPS];
};

/* 0 and `a' filled when `name' has a passwd line, else -1. */
int vlhe_conf_account(const char *name, struct vlhe_account *a);

/*
 * WHERE A USER'S OWN SETTINGS LIVE - THREE TIERS, 2026-09-24.
 *
 * THE USER'S DECISION, after a non-root user in portable mode found
 * that "I can okay and apply but nothing gets saved. Even the user
 * ones" (design/36 rows 59 and 60):
 *
 *   DEFAULT  portable: <tree>/vlhe-user-<name>.conf, one file PER
 *            USER, the tree's directory staged mode 1777 so anyone
 *            can create their own. Installed: $HOME/.vlhe/vlhe.conf.
 *   HOME     $HOME/.vlhe/vlhe.conf - offered, not assumed, when the
 *            default cannot be written. The GUI ASKS.
 *   TMP      /tmp/vlhe-<uid>/vlhe-user.conf - the last resort when
 *            $HOME fails too. Does not survive a reboot, and the
 *            caller must say so.
 *
 * THE NAME COMES FROM THE REAL UID, NEVER getlogin() OR THE EFFECTIVE
 * UID. getlogin() fails under X and sudo; the setuid build is euid 0
 * after the unlock and would name the file "root" for everyone.
 *
 * READ AND WRITE RESOLVE IN THE SAME ORDER, or a user who agreed to
 * $HOME once would lose their settings on the next launch. The read
 * rule: DEFAULT if its file exists or its directory can be written
 * (we would create it there - "the tree keeps its own", the
 * 2026-09-23 decision, holds whenever the tree is usable); otherwise
 * the first later tier whose file exists; otherwise DEFAULT.
 *
 * vlhe_conf_user_redirect() moves the CURRENT process to a tier after
 * the GUI has asked; it is in-memory, so the next launch re-resolves
 * and finds the file by the rule above.
 */
#define VLHE_USER_TIER_DEFAULT  0
#define VLHE_USER_TIER_HOME     1
#define VLHE_USER_TIER_TMP      2

int  vlhe_conf_user_name(char *out, int max);   /* login of the REAL uid */
const char *vlhe_conf_user_candidate(int tier); /* NULL if that tier has none */
int  vlhe_conf_user_tier(void);                 /* tier of the current path */
int  vlhe_conf_user_redirect(int tier);         /* 0, or -1 with errno */

/* Is a path writable by this process? Used for the "needs root"
 * greying - design/33 section 2 makes that decision on WRITABILITY
 * rather than on uid, which is also correct where an admin group owns
 * the file. A file that does not exist yet is writable if its
 * DIRECTORY is. */
int vlhe_conf_writable(const char *path);

/* Make the directory a path lives in, with mode 0755, parents
 * included. Returns 0 if it exists or was made. */
int vlhe_conf_mkdir_for(const char *path);

/*
 * OPEN A FRESH PRIVATE FILE IN /tmp - design/48 S6, design/49 N6.
 *
 * `stem' is the name's start ("vlhe-run"); the pid and a counter are
 * appended. The file is created with O_CREAT|O_EXCL and mode 0600, so
 * it is never one that already existed - not a file, not a symlink,
 * not a dangling link. On EEXIST the next counter is tried.
 *
 * Returns the descriptor with the path in `path', or -1 with `path'
 * EMPTIED - so a caller that removes `path' afterwards removes only
 * what this made. Removing a predictable /tmp name you did not create
 * is its own defect: as root it deletes whatever someone put there.
 */
int vlhe_tmp_open(char *path, int max, const char *stem);

#endif /* VLHE_BACKEND_CONF_H */
