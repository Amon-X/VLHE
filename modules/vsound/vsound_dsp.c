/*
 * vsound_dsp.c - the mapping, and the page walk under it.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/07-vsound.md sections 3 and 7. The reference's equivalent is
 * dsp.c, whose dsp_mmap_single() exposes bufsoft (dsp.c:2294) rather
 * than any hardware buffer - which is why every channel here is
 * mappable and none needs physical contiguity.
 *
 * WHAT IS PORTED, AND WHAT IS NOT.
 *
 * The nopage mechanism comes across from vcd/vcdsnd.c essentially
 * unchanged: it is the only piece of the old tree with a measurement
 * behind it (86Box, 2026-08-24 - three quake runs, all mapped, nopage
 * fired, same vm_start and buffer address each time). The page-walk
 * pair matches drivers/char/cpia.c:261-269 in the target's own tree.
 *
 * WHAT CHANGED, and it is the reason this is not a copy. The old
 * handler could not tell which channel a fault belonged to:
 *
 *     WHICH CHANNEL? nopage gets a vm_area_struct, not a file, so it
 *     cannot reach private_data the way mmap and write can. With one
 *     mapped client vcdsnd_chan_current() is that client and this is
 *     right. WITH TWO IT WOULD NOT BE - the second mapper would fault
 *     pages out of the first one's buffer.
 *
 * That is a correctness bug the moment two clients map, which is the
 * whole point of this rewrite. The comment proposed carrying the
 * channel in vma->vm_private_data.
 *
 * THAT FIELD DOES NOT EXIST ON 2.2. struct vm_area_struct
 * (include/linux/mm.h:34-60 in the target tree) has vm_mm, vm_start,
 * vm_end, vm_page_prot, vm_flags, the AVL and share links, vm_ops,
 * vm_offset, vm_file and vm_pte - and no private data field at all.
 * vm_private_data arrives in 2.4, so an example written for the later
 * kernel does not port.
 *
 * vm_file DOES exist, and mm/mmap.c:328 sets it from the mapping file
 * immediately after f_op->mmap returns. A fault happens later, so by
 * the time nopage runs vm_file is populated and vm_file->private_data
 * is the channel this open bound. That is the 2.2 route to the same
 * answer, and it is what makes N mapped clients correct here.
 */

#ifndef __KERNEL__
#error "vsound_dsp.c is kernel-only"
#endif

/*
 * NOT <linux/module.h>. Only ONE file in a module may include it: it
 * emits __module_kernel_version and __module_using_checksums into
 * .modinfo, and a second copy makes the partial link fail with
 * "multiple definition of `__module_kernel_version'".
 *
 * vsound_dev.c is that file, because it holds init_module(). This one
 * needs no module machinery - only the VM and the page tables.
 *
 * The old tree never met this: one .c per module, so the question did
 * not arise. It is the same class as the -Wno-unused-function
 * difference - a property of the real build that no host compile can
 * show.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <asm/pgtable.h>        /* pgd_offset_k, pte_offset - the walk */
#include <asm/io.h>             /* phys_to_virt */

#include "vsound_chan.h"

extern int vsound_trace;

/*
 * Walk the page tables to the kernel virtual address backing `adr'.
 *
 * drivers/char/cpia.c:238-269, in this kernel's own tree, and the
 * pattern CLAUDE.md section 4a names. ov511.c and ibmcam.c do the same.
 *
 * pte_page() on 2.2 returns an `unsigned long', not a `struct page *' -
 * the 2.4 signature would compute a wrong address here and the compiler
 * warns about it.
 *
 * Returns 0 when the page is not present, which callers must treat as
 * failure rather than as physical address zero.
 */
static inline unsigned long
vsound_uvirt_to_kva(pgd_t *pgd, unsigned long adr)
{
    unsigned long ret = 0UL;
    pmd_t *pmd;
    pte_t *ptep, pte;

    if (!pgd_none(*pgd)) {
        pmd = pmd_offset(pgd, adr);
        if (!pmd_none(*pmd)) {
            ptep = pte_offset(pmd, adr);
            pte  = *ptep;
            if (pte_present(pte))
                ret = pte_page(pte) | (adr & (PAGE_SIZE - 1));
        }
    }
    return ret;
}

static inline unsigned long
vsound_kvirt_to_pa(unsigned long adr)
{
    unsigned long va, kva;

    va  = VMALLOC_VMADDR(adr);
    kva = vsound_uvirt_to_kva(pgd_offset_k(va), va);
    return __pa(kva);
}

/*
 * Allocate a channel's buffer.
 *
 * vmalloc, NOT __get_free_pages. The old tree needed 64 KB of
 * PHYSICALLY CONTIGUOUS memory because it mapped with
 * remap_page_range(virt_to_phys(...)), which requires one extent - and
 * its own comment worried that an order-4 allocation may fail once
 * memory is fragmented. With nopage resolving pages one at a time that
 * requirement is gone, which is what makes a buffer PER CHANNEL
 * possible at all (07's section 4a, and 06's section 4a before it).
 *
 * The pages are marked reserved so the VM will not try to swap or free
 * them while userspace has them mapped - rvmalloc() in cpia.c:272-295
 * does exactly this.
 */
unsigned char *
vsound_dsp_alloc(unsigned int size)
{
    unsigned long adr;
    unsigned char *mem;

    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    mem  = (unsigned char *) vmalloc(size);
    if (mem == NULL)
        return NULL;

    memset(mem, 0, size);       /* no junk to userspace */

    adr = (unsigned long) mem;
    while (size > 0) {
        set_bit(PG_reserved,
                &mem_map[MAP_NR(phys_to_virt(vsound_kvirt_to_pa(adr)))].flags);
        adr  += PAGE_SIZE;
        size -= PAGE_SIZE;
    }
    return mem;
}

void
vsound_dsp_free(unsigned char *mem, unsigned int size)
{
    unsigned long adr;

    if (mem == NULL)
        return;

    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    adr  = (unsigned long) mem;
    while (size > 0) {
        clear_bit(PG_reserved,
                  &mem_map[MAP_NR(phys_to_virt(vsound_kvirt_to_pa(adr)))].flags);
        adr  += PAGE_SIZE;
        size -= PAGE_SIZE;
    }
    vfree(mem);
}

/*
 * Which channel does this mapping belong to?
 *
 * THE FIX THE OLD TREE COULD NOT MAKE. See the header: 2.2 has no
 * vm_private_data, but vm_file is set by mm/mmap.c:328 after
 * f_op->mmap returns, and a fault necessarily happens after that. So
 * the channel bound at open, in file->private_data, is reachable.
 *
 * Returns NULL rather than guessing. A fault we cannot attribute must
 * fail, not serve a page from whichever channel happens to be current -
 * that is precisely the bug being fixed, and quietly falling back to it
 * would reintroduce it under a different name.
 */
static struct vsound_chan *
vsound_dsp_vma_chan(struct vm_area_struct *vma)
{
    if (vma == NULL || vma->vm_file == NULL)
        return NULL;
    return (struct vsound_chan *) vma->vm_file->private_data;
}

/*
 * Resolve one page of the mapping, on fault.
 *
 * THE 2.2 SIGNATURE IS NOT THE 2.4 ONE. Here nopage returns an
 * `unsigned long' - the KERNEL VIRTUAL address of the page - and the
 * fault path (mm/memory.c:852-859) treats 0 as out-of-memory and -1 as
 * SIGBUS. 2.4 returns a `struct page *', so any example found for the
 * later kernel is the wrong shape.
 *
 * `address' is already page-aligned by the caller (`address & PAGE_MASK'
 * at mm/memory.c:852), but it is masked again rather than trusted: this
 * must not return a mid-page address under any circumstances.
 *
 * The offset is taken from vm_start rather than vm_offset because the
 * mmap below refuses any non-zero vm_offset, so a mapping always begins
 * at the start of the channel's buffer.
 */
static unsigned long
vsound_dsp_nopage(struct vm_area_struct *vma, unsigned long address,
                  int write_access)
{
    struct vsound_chan *c;
    unsigned long offset, kva, base;

    (void) write_access;

    c = vsound_dsp_vma_chan(vma);
    if (c == NULL || c->b.buf == NULL)
        return 0;

    address &= PAGE_MASK;
    if (address < vma->vm_start)
        return 0;
    offset = address - vma->vm_start;

    /*
     * Outside the buffer is a hard failure. It cannot happen while
     * mmap bounds-checks the size, but a VMA can be extended by
     * mremap() and this is the last line of defence - handing back a
     * page from beyond the buffer would expose unrelated kernel memory
     * to userspace.
     */
    if (offset >= c->b.bufsize)
        return 0;

    /*
     * vmalloc'd pages are scattered, so each is resolved individually -
     * that is the whole reason this handler exists rather than one
     * remap_page_range at mmap time.
     */
    base = (unsigned long) c->b.buf + offset;
    kva  = vsound_uvirt_to_kva(pgd_offset_k(VMALLOC_VMADDR(base)),
                               VMALLOC_VMADDR(base));
    if (kva == 0)
        return 0;

    kva &= PAGE_MASK;

    /*
     * NO REFERENCE IS TAKEN, AND ON 2.2 THAT IS THE CORRECT COUNT.
     *
     * design/25 B4, fixed 2026-09-14. This did
     * `atomic_inc(&mem_map[MAP_NR(kva)].count)', which is what 2.4's
     * nopage convention wants - and on 2.2 it leaked every page a
     * mapped client touched. The page is PG_reserved
     * (vsound_dsp_alloc), and 2.2's teardown skips reserved pages on
     * the DECREMENT: free_pte() (mm/memory.c:290-295) returns without
     * touching the count when PageReserved, and __free_pages()
     * (mm/page_alloc.c:128) applies the same rule. So the increment
     * here was never balanced: after the client closed,
     * vsound_dsp_free() cleared the bit and vfree()'s free_area_pte()
     * took the count from 1 + faults down to faults, never to zero,
     * and the page was off the free lists with nothing left to own it.
     * 4 KB per quake start (the only client that maps), the whole ring
     * for one that maps a larger SETFRAGMENT, until reboot.
     *
     * The reserved bit alone keeps the page from being freed or
     * swapped under the mapping, which is the protection the comment
     * this replaces was asking for. do_no_page installs the PTE and
     * zap_pte_range clears it; neither needs a count on a reserved
     * page. Verify on the target with /proc/meminfo across twenty
     * quake starts: MemFree must come back.
     */


    if (vsound_trace && offset == 0)
        printk(KERN_DEBUG "vsound: nopage: first page mapped"
                          " (vm_start %lx, chan %p, buf %p)\n",
               vma->vm_start, (void *) c, (void *) c->b.buf);

    return kva;
}

static struct vm_operations_struct vsound_dsp_vm_ops = {
    NULL,                   /* open         */
    NULL,                   /* close        */
    NULL,                   /* unmap        */
    NULL,                   /* protect      */
    NULL,                   /* sync         */
    NULL,                   /* advise       */
    vsound_dsp_nopage,      /* nopage       */
    NULL,                   /* wppage       */
    NULL,                   /* swapout      */
    NULL                    /* swapin       */
};

/*
 * Hand a channel's buffer to userspace.
 *
 * EVERY REFUSAL SAYS WHY. These returns were silent on the old tree and
 * that cost a diagnosis on 2026-08-24: quake mapped on one run in
 * three, the two that did not produced static, and the trace showed no
 * mmap line - which is indistinguishable from quake never calling mmap
 * at all. A refused mapping and an absent one need different fixes, and
 * nothing recorded which had happened.
 */
int
vsound_dsp_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct vsound_chan *c = (struct vsound_chan *) file->private_data;
    unsigned long size;

    if (c == NULL || c->b.buf == NULL) {
        if (vsound_trace)
            printk(KERN_DEBUG "vsound: mmap REFUSED -ENODEV"
                              " (no buffer - alloc failed at open?)\n");
        return -ENODEV;
    }
    if (!(vma->vm_flags & VM_WRITE)) {
        if (vsound_trace)
            printk(KERN_DEBUG "vsound: mmap REFUSED -EINVAL"
                              " (not mapped for write, flags 0x%x)\n",
                   (unsigned int) vma->vm_flags);
        return -EINVAL;
    }
    if (vma->vm_offset != 0) {
        if (vsound_trace)
            printk(KERN_DEBUG "vsound: mmap REFUSED -EINVAL"
                              " (offset %lu, must be 0)\n", vma->vm_offset);
        return -EINVAL;
    }

    size = vma->vm_end - vma->vm_start;
    if (size > (unsigned long) c->b.bufsize) {
        if (vsound_trace)
            printk(KERN_DEBUG "vsound: mmap REFUSED -EINVAL"
                              " (asked %lu, buffer is %u)\n",
                   size, c->b.bufsize);
        return -EINVAL;
    }

    /*
     * NO PAGES ARE MAPPED HERE. vm_ops->nopage resolves them on fault,
     * one at a time, which is what a vmalloc'd buffer requires.
     */
    vma->vm_ops = &vsound_dsp_vm_ops;

    c->flags |= VSOUND_CHN_MMAP;

    if (vsound_trace)
        printk(KERN_DEBUG "vsound: mmap %lu bytes (chan %p, buf %p)\n",
               size, (void *) c, (void *) c->b.buf);
    return 0;
}
