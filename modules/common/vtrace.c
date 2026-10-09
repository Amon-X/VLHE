/*
 * vtrace.c - the kernel half of a module's trace: the lock, the clock,
 * and /proc/<module>-trace with its own file operations. Included once
 * per module after VTRACE_MOD is defined; see vtrace.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * THE MODEL IS drivers/isdn/divert/divert_procfs.c (2.2.16): a module
 * gives a proc entry its own inode_operations whose default_file_ops
 * carry a read that sleeps, a poll, and an open that takes the module
 * use count - so an open reader pins the module and rmmod is refused
 * rather than oopsed (design/54 8f). /proc/kmsg is the model for the
 * WAIT only: its read consumes one global position, ours gives every
 * reader its own (8e), so `cat' by hand never steals from the capture.
 *
 * THE RING IS ALLOCATED ONLY WHEN TRACING IS ON - vmalloc at start(),
 * nothing otherwise - and the lines are text, formatted here under the
 * lock into a bounded buffer: the same vsprintf printk pays, and then
 * `cat' is the whole tool. The flood printk caused was not the
 * formatting; it was klogd and syslogd syncing every line to disk.
 *
 * LOCKING: spin_lock_irqsave, which on this uniprocessor kernel is the
 * cli() the callers already hold in the mix path; the add is a format,
 * a copy and a wake-up. The reader copies to user space OUTSIDE the
 * lock, from a bounce buffer filled under it.
 *
 * C89, GCC 2.95.2, kernel 2.2.16.
 */

#include <stdarg.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/malloc.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <asm/spinlock.h>

#include "vtrace.h"
#include "vtrace_core.c"

#define VTRACE_FMT_MAX   256    /* one formatted line, before the cut */
#define VTRACE_BOUNCE    512    /* per read() call, lines copied at once */

struct VT(state) {
    int                    on;              /* started, ring in place */
    struct vtrace_ring     ring;
    char                  *mem;
    const char            *name;            /* "vsound"               */
    char                   procname[32];    /* "vsound-trace"         */
    struct proc_dir_entry *ent;
    struct wait_queue     *wq;
    spinlock_t             lock;
    char                   fmt[VTRACE_FMT_MAX];
};

static struct VT(state) VT(st);
static struct inode_operations VT(iops);

/* ------------------------------------------------------------------ */
/* the writer                                                         */
/* ------------------------------------------------------------------ */

void
VT(printf)(const char *fmt, ...)
{
    struct VT(state) *t = &VT(st);
    struct timeval tv;
    unsigned long flags;
    va_list ap;
    char head[48];
    int n, h;

    if (!t->on || t->mem == NULL)
        return;
    spin_lock_irqsave(&t->lock, flags);

    /* THE MESSAGE FIRST, prefix-free; the head goes in front of it. */
    va_start(ap, fmt);
    /* vsprintf IS UNBOUNDED; the formats are this module's own and
     * short, and the buffer leaves 160 bytes where a slot holds 94.
     * The cut happens in the ring, not here. */
    n = vsprintf(t->fmt, fmt, ap);
    va_end(ap);
    /* A trailing newline in a converted printk format is the reader's
     * line end, not ours to store. */
    while (n > 0 && t->fmt[n - 1] == '\n')
        t->fmt[--n] = '\0';

    do_gettimeofday(&tv);
    h = sprintf(head, "%lu %lu.%06lu %s: ", t->ring.seq,
                (unsigned long) tv.tv_sec, (unsigned long) tv.tv_usec,
                t->name);
    /* The slot takes the head and the message as one line; the ring
     * cuts what does not fit. Two copies into the slot would need the
     * core to know about heads, so the head is pushed in front of the
     * message in the one buffer instead. */
    if (h + n > VTRACE_FMT_MAX - 1)
        n = VTRACE_FMT_MAX - 1 - h;
    memmove(t->fmt + h, t->fmt, (size_t) n);
    memcpy(t->fmt, head, (size_t) h);
    vtrace_ring_put(&t->ring, t->fmt, (unsigned long) (h + n));
    spin_unlock_irqrestore(&t->lock, flags);
    wake_up_interruptible(&t->wq);
}

/* ------------------------------------------------------------------ */
/* the file                                                           */
/* ------------------------------------------------------------------ */

static int
VT(open)(struct inode *inode, struct file *file)
{
    struct VT(state) *t = &VT(st);
    struct vtrace_pos *p;
    unsigned long flags;

    (void) inode;
    p = kmalloc(sizeof *p, GFP_KERNEL);
    if (p == NULL)
        return -ENOMEM;
    spin_lock_irqsave(&t->lock, flags);
    vtrace_pos_init(p, &t->ring);
    spin_unlock_irqrestore(&t->lock, flags);
    file->private_data = p;
    MOD_INC_USE_COUNT;
    return 0;
}

static int
VT(release)(struct inode *inode, struct file *file)
{
    (void) inode;
    if (file->private_data != NULL) {
        kfree(file->private_data);
        file->private_data = NULL;
    }
    MOD_DEC_USE_COUNT;
    return 0;
}

static ssize_t
VT(read)(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    struct VT(state) *t = &VT(st);
    struct vtrace_pos *p = file->private_data;
    char bounce[VTRACE_BOUNCE];
    unsigned long flags, n;
    size_t done = 0;

    (void) ppos;
    if (p == NULL || t->mem == NULL)
        return 0;                   /* stopped under the reader: EOF */

    /* WAIT FOR A LINE, unless told not to. Interruptible, so the
     * capture's SIGTERM ends a blocked `cat' or `vlhe trace'. */
    for (;;) {
        int avail;

        spin_lock_irqsave(&t->lock, flags);
        avail = vtrace_ring_avail(&t->ring, p);
        spin_unlock_irqrestore(&t->lock, flags);
        if (avail)
            break;
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        interruptible_sleep_on(&t->wq);
        if (signal_pending(current))
            return -ERESTARTSYS;
        if (t->mem == NULL)
            return 0;
    }

    /* WHOLE LINES, AS MANY AS FIT - the user's count and the bounce
     * buffer both bound a pass; a line that fits neither is left for
     * the next call, which a `cat' makes at once. */
    while (done < count) {
        unsigned long room = count - done;

        if (room > sizeof bounce)
            room = sizeof bounce;
        spin_lock_irqsave(&t->lock, flags);
        n = vtrace_ring_read(&t->ring, p, bounce, room);
        spin_unlock_irqrestore(&t->lock, flags);
        if (n == 0)
            break;
        if (copy_to_user(buf + done, bounce, n))
            return done > 0 ? (ssize_t) done : -EFAULT;
        done += n;
    }
    return (ssize_t) done;
}

static unsigned int
VT(poll)(struct file *file, poll_table *wait)
{
    struct VT(state) *t = &VT(st);
    struct vtrace_pos *p = file->private_data;
    unsigned long flags;
    int avail = 0;

    poll_wait(file, &t->wq, wait);
    if (p != NULL && t->mem != NULL) {
        spin_lock_irqsave(&t->lock, flags);
        avail = vtrace_ring_avail(&t->ring, p);
        spin_unlock_irqrestore(&t->lock, flags);
    }
    return avail ? (POLLIN | POLLRDNORM) : 0;
}

/* REFUSED: the position is the reader's sequence number, not a byte
 * offset, and a seek would mean nothing. -ESPIPE as a pipe says. */
static loff_t
VT(llseek)(struct file *file, loff_t off, int whence)
{
    (void) file; (void) off; (void) whence;
    return -ESPIPE;
}

static struct file_operations VT(fops) = {
    VT(llseek),
    VT(read),
    NULL,                   /* write */
    NULL,                   /* readdir */
    VT(poll),
    NULL,                   /* ioctl */
    NULL,                   /* mmap */
    VT(open),
    NULL,                   /* flush */
    VT(release),
    NULL                    /* fsync */
};

/* ------------------------------------------------------------------ */
/* start and stop                                                     */
/* ------------------------------------------------------------------ */

int
VT(start)(const char *name, unsigned long nslots)
{
    struct VT(state) *t = &VT(st);

    if (t->on)
        return 0;                   /* already on */
    t->name = name;
    t->wq   = NULL;
    spin_lock_init(&t->lock);
    if (nslots < 16)
        nslots = 16;
    t->mem = vmalloc(nslots * VTRACE_SLOT);
    if (t->mem == NULL)
        return -ENOMEM;
    vtrace_ring_init(&t->ring, t->mem, nslots);

    sprintf(t->procname, "%.24s-trace", name);
    /* 0444: the reader runs as the vlhe account, not root (8f). The
     * entry's ops are ours - divert_procfs.c's arrangement - set after
     * create_proc_entry() has filled the rest in. */
    t->ent = create_proc_entry(t->procname, S_IFREG | S_IRUGO, NULL);
    if (t->ent == NULL) {
        vfree(t->mem);
        t->mem = NULL;
        return -ENOENT;
    }
    memset(&VT(iops), 0, sizeof VT(iops));
    VT(iops).default_file_ops = &VT(fops);
    t->ent->ops = &VT(iops);
    t->on = 1;
    return 0;
}

void
VT(stop)(void)
{
    struct VT(state) *t = &VT(st);
    unsigned long flags;

    t->on = 0;
    if (t->ent != NULL) {
        remove_proc_entry(t->procname, NULL);
        t->ent = NULL;
    }
    if (t->mem != NULL) {
        /* NO READER CAN BE ATTACHED HERE: open() took the use count,
         * so cleanup_module is not reached while one is. The lock is
         * against a writer on a timer that has not yet seen us go. */
        spin_lock_irqsave(&t->lock, flags);
        vtrace_ring_init(&t->ring, NULL, 0);
        spin_unlock_irqrestore(&t->lock, flags);
        vfree(t->mem);
        t->mem = NULL;
        wake_up_interruptible(&t->wq);
    }
}
